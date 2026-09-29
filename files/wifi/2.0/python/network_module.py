"""
network_module.py  (Archivo 1)
================================
POR QUÉ este módulo existe: aísla TODO lo relacionado con el socket TCP
(conectar, enviar, recibir, decodificar, verificar CRC) en un HILO EN
SEGUNDO PLANO, para que la interfaz gráfica (gui_module.py) nunca se
congele esperando datos de red -- Tkinter, al ser de un solo hilo, se
vuelve no-responsiva si el hilo principal se bloquea en una llamada de
socket.

CON QUÉ se comunica: con la ESP32 vía socket TCP (ella es el SERVIDOR;
esta clase es el CLIENTE que inicia la conexión), y con gui_module.py
EXCLUSIVAMENTE a través de dos queue.Queue (inbound_queue / outbound_queue)
-- nunca comparte objetos de Tkinter ni el socket directamente con la GUI.
Esa es la forma segura de cruzar datos entre hilos en Python que pide el
enunciado.
"""

import socket
import threading
import queue
import time
from typing import Optional

from protocol import build_command_frame, parse_and_validate_frame, FrameError


class NetworkClient:
    """Cliente TCP que corre en su propio hilo (threading.Thread).

    PARÁMETROS del constructor:
      host, port      -- dirección de la ESP32 (impresa por su log serial
                          tras conectarse a la Wi-Fi) y el puerto en el
                          que escucha (TCP_SERVER_PORT en network_config.h
                          del lado firmware).
      inbound_queue   -- cola por la que este hilo EMPUJA hacia la GUI:
          {"kind": "data",      "payload": <dict del JSON>}
          {"kind": "crc_error", "count": <int total acumulado>}
          {"kind": "status",    "message": <str, estado de conexion>}
      outbound_queue  -- cola por la que la GUI EMPUJA hacia este hilo:
          enteros (valores a enviar hacia la ESP32).

    POR QUÉ dos colas separadas (una por dirección) y no una sola: cada
    cola tiene un único productor y un único consumidor, lo que evita
    cualquier ambigüedad sobre "quién escribe, quién lee" y simplifica el
    razonamiento sobre concurrencia (no hace falta ningún Lock adicional:
    queue.Queue ya es thread-safe internamente).
    """

    def __init__(self, host: str, port: int,
                 inbound_queue: "queue.Queue", outbound_queue: "queue.Queue"):
        self.host = host
        self.port = port
        self.inbound_queue = inbound_queue
        self.outbound_queue = outbound_queue

        self._sock: Optional[socket.socket] = None
        self._stop_event = threading.Event()
        self._crc_error_count = 0

        # PARÁMETRO daemon=True: si la ventana de Tkinter se cierra y el
        # proceso principal termina, este hilo NO debe impedir que el
        # proceso salga (un hilo daemon se corta automáticamente junto
        # con el programa, sin necesidad de sincronizar su apagado).
        self._thread = threading.Thread(target=self._run, daemon=True)

    def start(self) -> None:
        """COMUNICACIÓN: arranca self._run() en un hilo del sistema
        operativo aparte del hilo principal (donde vive el mainloop() de
        Tkinter)."""
        self._thread.start()

    def stop(self) -> None:
        """Señaliza el hilo para que termine (usado al cerrar la ventana,
        ver main.py) y cierra el socket si estaba abierto -- cerrar el
        socket desbloquea cualquier recv()/connect() en curso."""
        self._stop_event.set()
        sock = self._sock
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass

    def _emit_status(self, message: str) -> None:
        print(f"[network] {message}")
        self.inbound_queue.put({"kind": "status", "message": message})

    def _run(self) -> None:
        """Bucle de vida del hilo: reintenta conectar indefinidamente
        (con espera entre intentos) hasta que _stop_event se active."""
        while not self._stop_event.is_set():
            try:
                self._connect_and_serve()
            except (ConnectionRefusedError, OSError, socket.timeout) as exc:
                # PARÁMETRO time.sleep(3): evita un bucle de reconexión
                # "furioso" (sin pausa) si la ESP32 todavía no arrancó o
                # la IP configurada está mal -- 3 s es suficiente para no
                # saturar la red/CPU sin hacer esperar demasiado al usuario.
                self._emit_status(
                    f"No se pudo conectar a {self.host}:{self.port} ({exc}). "
                    f"Reintentando en 3 s..."
                )
                time.sleep(3)

    def _connect_and_serve(self) -> None:
        self._emit_status(f"Conectando a {self.host}:{self.port}...")

        # PARÁMETROS de socket.create_connection((host, port), timeout=5):
        # abre un socket TCP CLIENTE (la ESP32 es el servidor que escucha
        # en TCP_SERVER_PORT) con un timeout de 5 s solo para la fase de
        # conexión -- evita quedar colgado indefinidamente si la IP
        # configurada no responde.
        with socket.create_connection((self.host, self.port), timeout=5) as sock:
            # PARÁMETRO settimeout(0.5): a partir de aquí, cada recv()
            # individual espera como máximo 0.5 s. No es para "esperar
            # datos" (eso lo maneja el bucle de abajo) sino para poder
            # revisar outbound_queue y self._stop_event periódicamente,
            # en vez de bloquear este hilo para siempre en un único recv().
            sock.settimeout(0.5)
            self._sock = sock
            self._emit_status(f"Conectado a la ESP32 ({self.host}:{self.port})")

            buffer = ""

            while not self._stop_event.is_set():
                self._flush_outbound(sock)

                try:
                    # PARÁMETRO 4096: tamaño máximo de lectura por
                    # llamada -- más que suficiente para varias tramas de
                    # este protocolo (~100 B cada una); no es el tamaño
                    # "real" de lo que llega, solo un límite superior.
                    chunk = sock.recv(4096)
                except socket.timeout:
                    continue  # nada que leer en estos 0.5 s: reintenta
                except OSError:
                    break  # socket cerrado (p. ej. por stop()): sale del bucle

                if not chunk:
                    # recv() devuelve b"" cuando el otro lado cerró la
                    # conexión de forma ordenada (equivalente a recibir 0
                    # bytes en C).
                    self._emit_status("La ESP32 cerro la conexion")
                    break

                buffer += chunk.decode("utf-8", errors="replace")

                # Framing por línea: puede haber 0, 1 o varias tramas
                # completas en 'buffer' según cómo el sistema operativo
                # haya agrupado los bytes -- se procesan todas las que
                # ya tengan su '\n', y el resto (trama incompleta) queda
                # en 'buffer' para la próxima vuelta.
                while "\n" in buffer:
                    line, buffer = buffer.split("\n", 1)
                    line = line.strip()
                    if line:
                        self._handle_line(line)

        self._sock = None

    def _flush_outbound(self, sock: socket.socket) -> None:
        """Envía TODOS los valores que la GUI haya encolado desde la
        última vuelta del bucle, sin bloquear si no hay ninguno."""
        try:
            while True:
                # PARÁMETRO get_nowait(): NUNCA bloquea -- si la cola está
                # vacía, lanza queue.Empty de inmediato (capturada abajo).
                # Es lo que permite a este mismo bucle también revisar la
                # red sin quedar atascado esperando que la GUI mande algo.
                value = self.outbound_queue.get_nowait()
                frame = build_command_frame(value)
                sock.sendall(frame)
                print(f"[network] Enviado a la ESP32: {frame!r}")
        except queue.Empty:
            pass

    def _handle_line(self, line: str) -> None:
        """Decodifica y valida una línea recibida; actualiza el contador
        de errores de CRC del LADO PC y notifica a la GUI vía inbound_queue."""
        try:
            data = parse_and_validate_frame(line)
        except FrameError as exc:
            print(f"[network] Trama descartada (formato invalido): {exc}")
            return

        if not data.get("_crc_ok", False):
            self._crc_error_count += 1
            print("[network] === ERROR DE CRC EN TRAMA RECIBIDA ===")
            print(f"[network]   CRC recibido : {data['_crc_received']:04X}")
            print(f"[network]   CRC calculado: {data['_crc_computed']:04X}")
            print(f"[network]   Total de errores de CRC (lado PC): {self._crc_error_count}")
            self.inbound_queue.put({"kind": "crc_error", "count": self._crc_error_count})
            return

        self.inbound_queue.put({"kind": "data", "payload": data})
