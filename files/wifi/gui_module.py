"""
gui_module.py
-------------
Interfaz grafica (Tkinter) que muestra en tiempo real los datos recibidos
desde la ESP32 y el contador de errores de CRC, y permite enviar un valor
numerico hacia la ESP32.

Regla de oro de este modulo: la GUI JAMAS toca el socket directamente, y el
hilo de red JAMAS toca un widget de Tkinter (Tkinter no es thread-safe).
Toda la comunicacion pasa por inbound_queue / outbound_queue.
"""

import queue
import tkinter as tk
from tkinter import ttk

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
        self.value_entry.bind("<Return>", lambda _evt: self._on_send())

        ttk.Button(send_frame, text="Enviar", command=self._on_send).pack(side="left", padx=4)

        self.send_feedback_var = tk.StringVar(value="")
        ttk.Label(send_frame, textvariable=self.send_feedback_var).pack(side="left", padx=8)

    def _on_send(self) -> None:
        raw = self.value_entry.get().strip()

        try:
            value = int(raw)
        except ValueError:
            self.send_feedback_var.set("Ingresa un numero entero valido")
            return

        self.outbound_queue.put(value)
        self.send_feedback_var.set(f"Enviado: {value}")
        self.value_entry.delete(0, tk.END)

    def _append_data(self, payload: dict) -> None:
        self.data_text.configure(state="normal")
        self.data_text.delete("1.0", tk.END)
        for key, val in payload.items():
            if key.startswith("_crc"):
                continue
            self.data_text.insert(tk.END, f"{key}: {val}\n")
        self.data_text.configure(state="disabled")

    def _poll_inbound_queue(self) -> None:
        try:
            while True:
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
            self.after(POLL_INTERVAL_MS, self._poll_inbound_queue)
