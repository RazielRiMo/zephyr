"""
network_module.py
------------------
Modulo de red (Archivo 1): gestiona la conexion TCP con la ESP32 en un
hilo en segundo plano (background thread) para no bloquear la interfaz
grafica, valida el CRC de las tramas JSON recibidas y se comunica con la
GUI EXCLUSIVAMENTE a traves de queue.Queue (nunca acceden ambos lados al
mismo objeto directo -- esa es la forma segura de cruzar datos entre hilos
en Python que pide el enunciado).
"""

import socket
import threading
import queue
import time
from typing import Optional

from protocol import build_command_frame, parse_and_validate_frame, FrameError


class NetworkClient:
    """Cliente TCP que corre en su propio hilo.

    inbound_queue recibe dicts como:
      {"kind": "data",      "payload": <dict del JSON>}
      {"kind": "crc_error", "count": <int total acumulado>}
      {"kind": "status",    "message": <str, para mostrar el estado de conexion>}

    outbound_queue recibe enteros: valores a enviar hacia la ESP32.
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

        self._thread = threading.Thread(target=self._run, daemon=True)

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
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
        while not self._stop_event.is_set():
            try:
                self._connect_and_serve()
            except (ConnectionRefusedError, OSError, socket.timeout) as exc:
                self._emit_status(
                    f"No se pudo conectar a {self.host}:{self.port} ({exc}). "
                    f"Reintentando en 3 s..."
                )
                time.sleep(3)

    def _connect_and_serve(self) -> None:
        self._emit_status(f"Conectando a {self.host}:{self.port}...")

        with socket.create_connection((self.host, self.port), timeout=5) as sock:
            # Timeout corto (no None) para poder revisar outbound_queue y
            # el evento de parada periodicamente, sin bloquear para siempre
            # en un unico recv().
            sock.settimeout(0.5)
            self._sock = sock
            self._emit_status(f"Conectado a la ESP32 ({self.host}:{self.port})")

            buffer = ""

            while not self._stop_event.is_set():
                self._flush_outbound(sock)

                try:
                    chunk = sock.recv(4096)
                except socket.timeout:
                    continue
                except OSError:
                    break

                if not chunk:
                    self._emit_status("La ESP32 cerro la conexion")
                    break

                buffer += chunk.decode("utf-8", errors="replace")

                while "\n" in buffer:
                    line, buffer = buffer.split("\n", 1)
                    line = line.strip()
                    if line:
                        self._handle_line(line)

        self._sock = None

    def _flush_outbound(self, sock: socket.socket) -> None:
        try:
            while True:
                value = self.outbound_queue.get_nowait()
                frame = build_command_frame(value)
                sock.sendall(frame)
                print(f"[network] Enviado a la ESP32: {frame!r}")
        except queue.Empty:
            pass

    def _handle_line(self, line: str) -> None:
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
