"""
main.py
=======
POR QUÉ este módulo existe: es el punto de entrada que "cablea" las dos
piezas modulares (network_module.py y gui_module.py) a través de las colas
compartidas -- ninguna de las dos se conoce entre sí directamente.

Uso:
    python main.py [ip_esp32] [puerto]

Si no se indican argumentos, se usan los valores por defecto de abajo.
AJUSTA DEFAULT_HOST a la IP que la ESP32 imprime por el log serial al
conectarse a la Wi-Fi (ver wifi_manager.c -> "Direccion IP obtenida...").
Host y puerto son, del lado PC, el equivalente a lo que network_config.h
es del lado firmware: configurables desde el propio main, sin tocar la
lógica interna.
"""

import sys
import queue

from network_module import NetworkClient
from gui_module import App

DEFAULT_HOST = "192.168.1.50"
DEFAULT_PORT = 5000


def main() -> None:
    # PARÁMETROS por línea de comandos: sys.argv[1]/[2] permiten cambiar
    # host/puerto SIN editar el archivo, útil para probar contra varias
    # ESP32 sin recompilar nada del lado PC.
    host = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_HOST
    port = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_PORT

    # COMUNICACIÓN: estas dos colas son el ÚNICO canal entre el hilo de
    # red (NetworkClient) y el hilo de Tkinter (App) -- ver la
    # justificación detallada en network_module.py y gui_module.py.
    inbound_queue: "queue.Queue" = queue.Queue()
    outbound_queue: "queue.Queue" = queue.Queue()

    client = NetworkClient(host, port, inbound_queue, outbound_queue)
    client.start()  # arranca el hilo de red EN SEGUNDO PLANO (no bloquea aquí)

    app = App(inbound_queue, outbound_queue)

    # PARÁMETROS de app.protocol("WM_DELETE_WINDOW", callback): intercepta
    # el evento "cerrar ventana" (la X) para primero detener limpiamente
    # el hilo de red (client.stop(), que cierra el socket) y recién
    # después destruir la ventana -- evita dejar el hilo de red "colgado"
    # cuando el usuario cierra la GUI.
    app.protocol("WM_DELETE_WINDOW", lambda: (client.stop(), app.destroy()))

    # COMUNICACIÓN: mainloop() es el bucle de eventos de Tkinter -- corre
    # en ESTE hilo (el principal) y es quien realmente invoca
    # _poll_inbound_queue() cada POLL_INTERVAL_MS mediante los after()
    # programados dentro de App.
    app.mainloop()


if __name__ == "__main__":
    main()
