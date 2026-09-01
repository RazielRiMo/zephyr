# TM1638 en ESP32 con MicroPython

Librería simple para el módulo TM1638 (8 displays de 7 segmentos + 8
LEDs + 8 botones) en MicroPython, usando únicamente `machine.Pin` para
hacer bit-banging del protocolo. Cero dependencias externas.

Es el mismo conjunto de funciones que la versión para Zephyr/C de este
mismo proyecto, adaptado al estilo de Python — ver la sección 8 más
abajo para las diferencias de diseño entre ambas.

## 1. Archivos

```
tm1638_esp32/
├── tm1638.py    # la librería
└── main.py      # ejemplo de uso de cada función
```

## 2. Prerrequisito

Este README asume que tu ESP32 ya tiene el firmware de MicroPython
instalado (no un ESP32 "en blanco" con el firmware de fábrica de
Espressif). Si todavía no lo tienes, instálalo primero con `esptool`
y el `.bin` de https://micropython.org/download/ — es un paso previo
independiente de esta librería.

## 3. Cómo subir los archivos al ESP32

MicroPython ejecuta automáticamente `boot.py` (si existe) y luego
`main.py` cada vez que la placa arranca o se resetea. Necesitas copiar
`tm1638.py` y `main.py` a la **raíz** del sistema de archivos interno
del ESP32 (no a una carpeta). Cualquiera de estas opciones funciona:

- **Thonny** (más fácil para empezar): conecta el ESP32, abre cada
  archivo y usa "Archivo → Guardar copia como → MicroPython device",
  o arrastra los archivos en el panel de archivos del dispositivo.
- **mpremote** (herramienta oficial de línea de comandos):
  ```bash
  mpremote connect <puerto> cp tm1638.py :
  mpremote connect <puerto> cp main.py :
  mpremote connect <puerto> reset
  ```
- **ampy** / **rshell**: herramientas alternativas de línea de
  comandos con el mismo propósito (`ampy put tm1638.py`, etc.).
- **WebREPL**: si lo tienes habilitado, puedes arrastrar los archivos
  por WiFi sin cable.

Tras copiarlos, resetea la placa (botón EN/RST, o `mpremote reset`) y
`main.py` arranca solo.

## 4. Cableado

El ejemplo de `main.py` usa estos pines (elegidos por ser de uso
general en la mayoría de placas ESP32 DevKit, sin funciones especiales
de arranque):

| TM1638 | ESP32   |
|--------|---------|
| VCC    | 3V3 o 5V (según tu módulo; la mayoría de placas TM1638 aceptan ambos) |
| GND    | GND     |
| STB    | GPIO4   |
| CLK    | GPIO18  |
| DIO    | GPIO19  |

Para usar otros pines, solo cambia los números al crear el objeto:

```python
tm = TM1638(stb=4, clk=18, dio=19)
```

**Evita** los pines de arranque/strapping del ESP32 (GPIO0, GPIO2,
GPIO5, GPIO12, GPIO15) y los pines de solo entrada (GPIO34-39, que no
sirven aquí porque STB/CLK/DIO necesitan poder actuar como salida).

## 5. Protocolo del TM1638 (cómo funciona por dentro)

- **3 líneas**: `STB` (chip select, activo en bajo durante cada
  transacción), `CLK` (reloj que generamos nosotros) y `DIO` (datos,
  **bidireccional**).
- **DIO bidireccional**: cada método de bajo nivel reconfigura el pin
  justo antes de usarlo — `self._dio.init(Pin.OUT)` para transmitir,
  `Pin.IN` (con pull-up) para los 4 bytes que el TM1638 devuelve al
  leer el teclado. Esto pasa dentro de `_send_byte()` / `_recv_byte()`
  en `tm1638.py`, nunca lo tienes que manejar desde `main.py`.
- **Escritura**: `STB` en bajo, se manda `0x44` (modo dirección fija),
  `STB` en alto; luego `STB` en bajo otra vez, se manda
  `0xC0 | dirección` seguido del byte de datos, `STB` en alto. Cada
  dirección (0-15) es un byte de la memoria de display: la par `i*2`
  es el dígito `i`, la impar `i*2+1` es su LED asociado (así están
  cableados los módulos "LED&KEY" estándar, los más comunes).
- **Brillo**: un solo byte `0x88 | nivel` (bit 3 = pantalla encendida,
  bits 0-2 = brillo 0-7).
- **Lectura de botones**: `STB` en bajo, se manda `0x42`, se cambia
  `DIO` a entrada y se leen 4 bytes (cada uno trae el estado de 2
  botones, en el bit 0 y el bit 4), `STB` en alto, `DIO` vuelve a
  salida para la próxima escritura.

## 6. Mapeo de segmentos

`tm.set_digit(digit, segments)` usa el orden de bits que el propio
TM1638 espera (no hace falta reordenar nada):

```
bit 0 = a      bit 4 = e
bit 1 = b      bit 5 = f
bit 2 = c      bit 6 = g
bit 3 = d      bit 7 = dp (punto decimal)
```

`0xFF` → los 7 segmentos + el punto decimal encendidos.
`0x00` → todos los segmentos apagados.

## 7. Manejo de errores

A diferencia de la versión en C (que devuelve códigos como `-EINVAL`),
aquí los argumentos fuera de rango lanzan `ValueError` — es el estilo
idiomático de Python y evita tener que revisar manualmente un código
de retorno después de cada llamada:

| Método | Lanza `ValueError` si... |
|---|---|
| `set_brightness(level)` | `level` no está entre 0 y 7 |
| `set_digit(digit, segments)` | `digit` no está entre 0 y 7 |
| `display(value)` | `value` no está entre 0 y 99999999 (incluye negativos) |
| `set_led(led, state)` | `led` no está entre 0 y 7, o `state` no es 0 ni 1 |

`display()` con un valor fuera de rango además dibuja una fila de
guiones `"--------"` en el módulo antes de lanzar la excepción, para
que el error también sea visible en el hardware.

## 8. Debounce de botones: cómo funciona

`get_button()` implementa un antirrebote **no bloqueante** (nunca
llama a `time.sleep()` dentro de sí mismo), usando
`time.ticks_ms()` / `time.ticks_diff()`:

1. Cada vez que se llama, lee el estado crudo de los 8 botones.
2. Si ese estado crudo cambió respecto a la lectura anterior, guarda
   el nuevo valor y reinicia un cronómetro — pero **todavía no lo
   acepta** como válido, porque podría ser rebote mecánico.
3. Solo cuando el estado crudo lleva **30 ms sin volver a cambiar**,
   se copia a un segundo atributo interno, que es lo que la función
   realmente reporta.

Por eso hay que llamarla periódicamente (en `main.py`, cada 20 ms
dentro del `while True`): el "asentado" del antirrebote pasa entre
llamadas, no dentro de una sola llamada.

Se usa `time.ticks_ms()` + `time.ticks_diff()` en vez de restar
timestamps directamente porque `ticks_ms()` puede dar la vuelta
(overflow) tras un tiempo de uso continuo; `ticks_diff()` calcula la
diferencia correctamente incluso si eso pasó — es la forma correcta y
recomendada por la documentación de MicroPython para medir intervalos.

**Varios botones a la vez**: si hay más de uno presionado en el mismo
instante, se devuelve el de menor número (el más a la izquierda), ya
que la función solo puede comunicar un botón por llamada.

## 9. Diferencias de diseño respecto a la versión Zephyr/C

- **Clase en vez de funciones sueltas con estado estático.** La
  versión en C usaba variables `static` dentro de `tm1638.c` para
  poder llamar `tm1638_display(numero)` sin pasar un handle. En Python
  no hace falta ese truco: una clase ya te da un objeto con su propio
  estado (`self`), así que `tm.display(numero)` es igual de simple
  *y* además permite manejar varios módulos TM1638 a la vez si algún
  día lo necesitas (creando varias instancias de `TM1638` con pines
  distintos) — algo que la versión en C, tal como está, no permite.
- **Excepciones en vez de códigos de retorno.** Ver la sección 7.
- **`time.sleep_us()` / `time.sleep_ms()` en vez de `k_busy_wait()` /
  `k_msleep()`.** Mismo propósito, API equivalente de MicroPython.
- **`time.ticks_ms()` / `time.ticks_diff()` en vez de `k_uptime_get()`**
  para el antirrebote — ambas dan un timestamp monotónico creciente en
  milisegundos, usado exactamente de la misma forma.
