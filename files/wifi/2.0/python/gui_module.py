"""
gui_module.py  (Archivo 2)
============================
POR QUÉ este módulo existe: es la interfaz Tkinter que pide el enunciado,
aislada del código de red -- solo sabe "mostrar lo que llega" y "encolar lo
que el usuario quiere enviar", nunca cómo se logra la conexión TCP.

REGLA DE ORO de este módulo (y la razón técnica detrás): Tkinter NO es
thread-safe -- sus widgets solo deben tocarse desde el hilo que corrió
mainloop() (el hilo principal). Por eso esta clase JAMÁS toca el socket
directamente, y network_module.py JAMÁS importa ni referencia un widget.
Toda la comunicación pasa por inbound_queue / outbound_queue.

CON QUÉ se comunica: con network_module.NetworkClient, exclusivamente vía
las dos queue.Queue que le pasa main.py.
"""

import queue
import tkinter as tk
from tkinter import ttk

# PARÁMETRO POLL_INTERVAL_MS = 100: cada cuánto el hilo de Tkinter revisa
# inbound_queue (ver _poll_inbound_queue). 100 ms es imperceptible para un
# humano pero suficientemente espaciado para no consumir CPU en un bucle
# ajustado -- es el mecanismo estándar de Tkinter para "recibir" datos de
# otro hilo sin bloquear el mainloop().
POLL_INTERVAL_MS = 100


class App(tk.Tk):
    def __init__(self, inbound_queue: "queue.Queue", outbound_queue: "queue.Queue"):
        super().__init__()

        self.inbound_queue = inbound_queue
        self.outbound_queue = outbound_queue

        self.title("Puente TCP/JSON - ESP32 (Zephyr RTOS)")
        self.geometry("540x440")
        self.resizable(False, False)

        self._build_widgets()

        # COMUNICACIÓN: self.after(ms, func) programa que Tkinter llame a
        # 'func' dentro de SU PROPIO hilo (el principal) una vez pasado el
        # intervalo -- es la forma segura de "revisar" una cola llenada
        # por otro hilo sin violar la regla de un solo hilo de Tkinter.
        self.after(POLL_INTERVAL_MS, self._poll_inbound_queue)

    def _build_widgets(self) -> None:
        pad = {"padx": 10, "pady": 6}

        status_frame = ttk.LabelFrame(self, text="Estado de conexion")
        status_frame.pack(fill="x", **pad)
        self.status_var = tk.StringVar(value="Iniciando...")
        ttk.Label(status_frame, textvariable=self.status_var).pack(anchor="w", padx=8, pady=4)

        data_frame = ttk.LabelFrame(self, text="Ultima trama recibida (JSON)")
        data_frame.pack(fill="both", expand=True, **pad)
        self.data_text = tk.Text(data_frame, height=10, state="disabled", wrap="word")
        self.data_text.pack(fill="both", expand=True, padx=8, pady=4)

        stats_frame = ttk.LabelFrame(self, text="Estadisticas")
        stats_frame.pack(fill="x", **pad)
        self.crc_error_var = tk.StringVar(value="Errores de CRC acumulados (lado PC): 0")
        ttk.Label(stats_frame, textvariable=self.crc_error_var).pack(anchor="w", padx=8, pady=4)

        send_frame = ttk.LabelFrame(self, text="Enviar valor numerico a la ESP32")
        send_frame.pack(fill="x", **pad)

        self.value_entry = ttk.Entry(send_frame, width=12)
        self.value_entry.pack(side="left", padx=8, pady=8)
        # COMUNICACIÓN: <Return> liga la tecla Enter, dentro del campo de
        # texto, al mismo manejador que el botón -- comodidad de uso, no
        # cambia la lógica de envío.
        self.value_entry.bind("<Return>", lambda _evt: self._on_send())

        ttk.Button(send_frame, text="Enviar", command=self._on_send).pack(side="left", padx=4)

        self.send_feedback_var = tk.StringVar(value="")
        ttk.Label(send_frame, textvariable=self.send_feedback_var).pack(side="left", padx=8)

    def _on_send(self) -> None:
        """Se ejecuta en el hilo de Tkinter (por el binding del botón/tecla
        Enter). Valida la entrada y la pone en outbound_queue -- NUNCA
        llama directamente a build_command_frame() ni toca un socket aquí:
        eso es responsabilidad exclusiva de network_module.py, que
        consumirá este valor desde SU hilo."""
        raw = self.value_entry.get().strip()

        try:
            value = int(raw)
        except ValueError:
            self.send_feedback_var.set("Ingresa un numero entero valido")
            return

        # COMUNICACIÓN: outbound_queue.put() es thread-safe por diseño de
        # queue.Queue -- no hace falta ningún Lock manual para que
        # network_module.py lo recoja de forma segura desde su propio hilo.
        self.outbound_queue.put(value)
        self.send_feedback_var.set(f"Enviado: {value}")
        self.value_entry.delete(0, tk.END)

    def _append_data(self, payload: dict) -> None:
        """Vuelca el contenido de un JSON recibido en el cuadro de texto.
        PARÁMETRO: se filtran las claves que empiezan con "_crc" porque son
        METADATOS que protocol.py agregó para uso interno (resultado de la
        verificación), no campos que la ESP32 haya enviado realmente."""
        self.data_text.configure(state="normal")
        self.data_text.delete("1.0", tk.END)
        for key, val in payload.items():
            if key.startswith("_crc"):
                continue
            self.data_text.insert(tk.END, f"{key}: {val}\n")
        self.data_text.configure(state="disabled")

    def _poll_inbound_queue(self) -> None:
        """Se re-programa a sí misma cada POLL_INTERVAL_MS (ver el
        self.after() al final). Drena TODOS los mensajes pendientes en
        cada pasada (no solo uno), para no acumular retraso si
        network_module.py encoló varios mientras la GUI estaba ocupada
        redibujando algo."""
        try:
            while True:
                # PARÁMETRO get_nowait(): nunca bloquea el hilo de
                # Tkinter -- si no hay nada, sale del bucle vía
                # queue.Empty en vez de congelar la interfaz esperando.
                msg = self.inbound_queue.get_nowait()
                kind = msg.get("kind")

                if kind == "data":
                    self._append_data(msg["payload"])
                elif kind == "crc_error":
                    self.crc_error_var.set(
                        f"Errores de CRC acumulados (lado PC): {msg['count']}"
                    )
                elif kind == "status":
                    self.status_var.set(msg["message"])
        except queue.Empty:
            pass
        finally:
            # Reprograma la próxima revisión -- si no se hiciera esto, el
            # sondeo se ejecutaría una sola vez y nunca más.
            self.after(POLL_INTERVAL_MS, self._poll_inbound_queue)
