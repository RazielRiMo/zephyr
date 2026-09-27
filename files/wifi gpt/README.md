# ESP32 + Zephyr RTOS + PC Python: TCP bidireccional, JSON y CRC

## Estructura

```text
zephyr_wifi_tcp_project/
├── zephyr_app/
│   ├── CMakeLists.txt
│   ├── prj.conf
│   └── src/
│       ├── config.h
│       └── main.c
└── pc_python/
    ├── network.py
    ├── gui.py
    └── README.md
```

## 1. Firmware ESP32

El proyecto está preparado para el flujo de Zephyr 4.4.x y para el objetivo:

```text
esp32_devkitc/esp32/procpu
```

La documentación actual de Zephyr identifica `esp32_devkitc` como la placa ESP32-DevKitC y su target `esp32_devkitc/esp32/procpu`. También requiere los blobs HAL de Espressif para Wi-Fi en la configuración correspondiente.

Desde el entorno de Zephyr:

```powershell
west blobs fetch hal_espressif
```

Edite `zephyr_app/src/config.h`:

```c
#define WIFI_SSID       "MiWiFi"
#define WIFI_PASSWORD   "MiPassword"
#define PC_SERVER_IP    "192.168.1.100"
#define PC_SERVER_PORT  5000
```

`PC_SERVER_IP` es la IPv4 del computador donde se ejecuta `gui.py`.

Construcción:

```powershell
cd C:\ruta\al\proyecto\zephyr_app
west build -p always -b esp32_devkitc/esp32/procpu .
west flash
west espressif monitor
```

Si tu versión de west no tiene el comando `west espressif monitor`, usa el monitor serie disponible en tu instalación o la terminal serie de tu elección.

## 2. PC

Python 3.10+ recomendado. No necesita paquetes externos: usa biblioteca estándar (`socket`, `threading`, `queue`, `tkinter`, `json`).

Desde `pc_python`:

```powershell
python gui.py
```

El servidor queda en:

```text
0.0.0.0:5000
```

## 3. Funcionamiento

### ESP32 -> PC

Cada `TX_PERIOD_MS` se genera una telemetría con `seq`, `value` y contador de errores CRC. Como `seq/value` cambian, también cambia el CRC.

Ejemplo:

```text
TX telemetry: {"type":"telemetry","seq":1,"value":27,"crc":....}
TX telemetry: {"type":"telemetry","seq":2,"value":34,"crc":....}
```

### PC -> ESP32

En la GUI se escribe, por ejemplo:

```text
123.5
```

Python lo convierte en:

```text
{"type":"command","value":123.5,"crc":....}
```

La ESP32 valida el CRC, decodifica el número y responde:

```text
{"type":"ack","status":0,"value":123.5,"crc_errors":0,"crc":....}
```

## 4. Sincronización RX

TCP es un stream, no un protocolo de mensajes. Por eso no se asume que cada `recv()` contiene un JSON completo. El firmware acumula bytes y separa mensajes con `\n`.

Después de completar una trama, `rx_event_hook()` libera el semáforo binario `rx_sem`. `rx_processing_thread()` queda bloqueado en:

```c
k_sem_take(&rx_sem, K_FOREVER);
```

y sólo entonces procesa las tramas pendientes de la `k_msgq`.

### Sobre la ISR solicitada

Una llegada TCP no es una fuente de interrupción GPIO accesible directamente por la aplicación. En Zephyr, la recepción de sockets ocurre mediante la pila de red y el hilo que ejecuta `recv()`; por eso aquí se implementa el equivalente funcional: evento de recepción -> semáforo binario -> tarea de procesamiento.

Si necesitas una ISR literal, debe existir una fuente hardware real (GPIO, UART, timer, etc.); utilizar una ISR ficticia para "simular" RX de TCP sería engañoso y no mejoraría el diseño.

## 5. CRC

Algoritmo en ambos lados:

```text
CRC-16-CCITT-FALSE
Polynomial = 0x1021
Initial    = 0xFFFF
RefIn      = false
RefOut     = false
XorOut     = 0x0000
```

El campo `crc` queda fuera del cuerpo CRC.

## 6. Prueba de error de CRC

La prueba más sencilla es modificar manualmente un valor de la trama que sale del ESP32 o del PC mientras se depura. Por ejemplo, cambiar:

```text
"value":27
```

a:

```text
"value":28
```

sin recalcular el CRC. El receptor debe mostrar una alerta y aumentar su contador local.

## 7. Salidas esperadas

### Terminal ESP32

```text
[INF] ESP32 Zephyr - WiFi/TCP/JSON/CRC
[INF] Conectando a SSID 'MiWiFi'...
[INF] Wi-Fi conectado
[INF] IPv4 asignada por DHCP: 192.168.1.50
[INF] Conectando TCP a 192.168.1.100:5000...
[INF] TCP conectado correctamente
[INF] TX telemetry: {"type":"telemetry","seq":1,"value":27,"crc":...}
[INF] TX telemetry: {"type":"telemetry","seq":2,"value":34,"crc":...}
[INF] RX CRC OK: 0x.... | body={"type":"command","value":123.500}
[INF] Comando recibido desde PC: value=123.500
[INF] TX ACK: {"type":"ack","status":0,"value":123.500,"crc_errors":0,"crc":...}
```

### GUI

- Estado de conexión.
- Último `value` recibido.
- Contador de errores CRC del lado PC.
- Campo numérico y botón Enviar.
- Log en tiempo real.
