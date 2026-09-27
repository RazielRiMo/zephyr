"""GUI Tkinter para monitoreo y envío de comandos al ESP32."""

from __future__ import annotations

import json
import math
import queue
import tkinter as tk
from tkinter import messagebox, ttk

from network import TcpServer, NetworkEvent


class App(tk.Tk):
    def __init__(self) -> None:
        super().__init__()

        self.title("ESP32 Zephyr - TCP JSON CRC")
        self.geometry("820x620")
        self.minsize(700, 500)

        self.network = TcpServer()

        self.status_var = tk.StringVar(value="Iniciando servidor TCP...")
        self.crc_var = tk.StringVar(value="0")
        self.last_value_var = tk.StringVar(value="--")
        self.command_var = tk.StringVar()

        self._build_widgets()
        self.protocol("WM_DELETE_WINDOW", self._on_close)

        self.network.start()
        self.after(100, self._poll_network_events)

    def _build_widgets(self) -> None:
        root = ttk.Frame(self, padding=12)
        root.pack(fill="both", expand=True)

        ttk.Label(
            root,
            text="Comunicación bidireccional ESP32 ↔ PC",
            font=("Segoe UI", 16, "bold"),
        ).pack(anchor="w")

        ttk.Label(root, textvariable=self.status_var).pack(anchor="w", pady=(4, 12))

        stats = ttk.LabelFrame(root, text="Estado", padding=10)
        stats.pack(fill="x", pady=(0, 10))

        ttk.Label(stats, text="Último value recibido:").grid(row=0, column=0, sticky="w")
        ttk.Label(stats, textvariable=self.last_value_var).grid(row=0, column=1, padx=8, sticky="w")

        ttk.Label(stats, text="Errores CRC PC:").grid(row=1, column=0, sticky="w", pady=(5, 0))
        ttk.Label(stats, textvariable=self.crc_var).grid(row=1, column=1, padx=8, sticky="w", pady=(5, 0))

        cmd = ttk.LabelFrame(root, text="Enviar comando numérico", padding=10)
        cmd.pack(fill="x", pady=(0, 10))

        entry = ttk.Entry(cmd, textvariable=self.command_var, width=25)
        entry.grid(row=0, column=0, padx=(0, 8))
        entry.bind("<Return>", self._send_value)

        ttk.Button(cmd, text="Enviar", command=self._send_value).grid(row=0, column=1)
        ttk.Label(
            cmd,
            text='Se envía: {"type":"command","value":N,"crc":...}',
        ).grid(row=1, column=0, columnspan=2, sticky="w", pady=(7, 0))

        log_frame = ttk.LabelFrame(root, text="Actividad", padding=8)
        log_frame.pack(fill="both", expand=True)

        self.log = tk.Text(log_frame, height=20, state="disabled", wrap="none")
        self.log.pack(side="left", fill="both", expand=True)

        scrollbar = ttk.Scrollbar(log_frame, orient="vertical", command=self.log.yview)
        scrollbar.pack(side="right", fill="y")
        self.log.configure(yscrollcommand=scrollbar.set)

    def _append_log(self, text: str) -> None:
        self.log.configure(state="normal")
        self.log.insert("end", text + "\n")
        self.log.see("end")
        self.log.configure(state="disabled")

    def _poll_network_events(self) -> None:
        # Tkinter debe actualizarse en el hilo principal.
        try:
            while True:
                event = self.network.events.get_nowait()
                self._handle_event(event)
        except queue.Empty:
            pass

        if self.winfo_exists():
            self.after(100, self._poll_network_events)

    def _handle_event(self, event: NetworkEvent) -> None:
        if event.kind == "status":
            self.status_var.set(event.message)
            self._append_log("[INFO] " + event.message)
        elif event.kind == "connected":
            self.status_var.set("ESP32 conectado")
            self._append_log("[CONEXIÓN] " + event.message)
        elif event.kind == "disconnected":
            self.status_var.set("Esperando conexión ESP32...")
            self._append_log("[DESCONEXIÓN] " + event.message)
        elif event.kind == "crc_error":
            self.crc_var.set(str(self.network.crc_error_count))
            self._append_log("[ERROR CRC] " + event.message)
        elif event.kind == "rx" and event.payload is not None:
            self._process_rx_payload(event.payload)
            self._append_log(event.message)
        elif event.kind == "tx":
            self._append_log(event.message)
        elif event.kind == "error":
            self._append_log("[ERROR] " + event.message)

        self.crc_var.set(str(self.network.crc_error_count))

    def _process_rx_payload(self, payload: dict) -> None:
        # Muestra cualquier value numérico que llegue.
        value = payload.get("value")
        if isinstance(value, (int, float)) and not isinstance(value, bool):
            self.last_value_var.set(str(value))

    def _send_value(self, event=None) -> None:
        raw = self.command_var.get().strip().replace(",", ".")
        if not raw:
            messagebox.showwarning("Dato faltante", "Digite un valor numérico.")
            return

        try:
            value = float(raw)
        except ValueError:
            messagebox.showerror("Valor inválido", "El valor debe ser numérico.")
            return

        if not math.isfinite(value):
            messagebox.showerror("Valor inválido", "No se permiten NaN ni infinito.")
            return

        try:
            self.network.send_numeric(value)
        except (ConnectionError, OSError) as exc:
            messagebox.showerror("TCP", str(exc))
            return

        self.command_var.set("")

    def _on_close(self) -> None:
        self.network.stop()
        self.destroy()


if __name__ == "__main__":
    app = App()
    app.mainloop()
