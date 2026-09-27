"""
main.py
-------
Punto de entrada de la aplicacion de PC. Crea las colas compartidas,
arranca el hilo de red (network_module.NetworkClient) y lanza la interfaz
grafica (gui_module.App).

Uso:
    python main.py [ip_esp32] [puerto]

Si no se indican argumentos, se usan los valores por defecto de mas abajo.
AJUSTA DEFAULT_HOST a la IP que la ESP32 imprime por el log serial al
conectarse a la Wi-Fi (ver wifi_manager.c -> "Direccion IP obtenida...").
"""

import sys
import queue

from network_module import NetworkClient
from gui_module import App

DEFAULT_HOST = "192.168.1.50"
DEFAULT_PORT = 5000


def main() -> None:
    host = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_HOST
    port = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_PORT

    inbound_queue: "queue.Queue" = queue.Queue()
    outbound_queue: "queue.Queue" = queue.Queue()

    client = NetworkClient(host, port, inbound_queue, outbound_queue)
    client.start()

    app = App(inbound_queue, outbound_queue)
    app.protocol("WM_DELETE_WINDOW", lambda: (client.stop(), app.destroy()))
    app.mainloop()


if __name__ == "__main__":
    main()
