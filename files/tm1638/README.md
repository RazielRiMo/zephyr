# TM1638 en Nucleo-F401RE con Zephyr

Librería simple y reutilizable en C para el módulo TM1638 (8 displays
de 7 segmentos + 8 LEDs + 8 botones), usando únicamente la API de GPIO
de Zephyr. Cero dependencias de Arduino (nada de `HAL`, `delay()`,
`digitalWrite()`, `pinMode()`).

## 1. Archivos y dónde colocarlos

```
tm1638_nucleo/
├── CMakeLists.txt          # no necesita cambios
├── prj.conf                # no necesita cambios
├── app.overlay              # define los 3 pines STB/CLK/DIO
└── src/
    ├── main.c              # ejemplo de uso de cada función
    ├── tm1638.h             # API pública de la librería
    └── tm1638.c             # implementación (protocolo bit-bang)
```

Copia toda la carpeta `tm1638_nucleo/` dentro de tu workspace de west,
al mismo nivel que tus otras aplicaciones (junto a la carpeta `zephyr/`
que clona `west init`).

## 2. Cómo incluir tm1638.h

Ya está resuelto en `src/main.c`:

```c
#include "tm1638.h"
```

Como `tm1638.c` se agrega como fuente en el mismo `CMakeLists.txt` de
la app (ver más abajo), no hace falta ningún paso adicional de
compilación ni de `west`: son archivos de la propia aplicación, no un
módulo externo.

## 3. Cableado

| TM1638 | Nucleo (header Arduino) |
|--------|--------------------------|
| VCC    | 5V                       |
| GND    | GND                      |
| STB    | D2                       |
| CLK    | D4                       |
| DIO    | D7                       |

## 4. Devicetree: cómo está configurado y cómo cambiar los pines

`app.overlay`, en la raíz del proyecto (no dentro de `boards/`), usa el
nodo estándar `zephyr,user` para exponer 3 GPIO sueltos a la
aplicación sin necesitar un binding propio:

```dts
#include <zephyr/dt-bindings/gpio/gpio.h>

/ {
	zephyr,user {
		tm1638-stb-gpios = <&arduino_header 2 GPIO_ACTIVE_HIGH>;
		tm1638-clk-gpios = <&arduino_header 4 GPIO_ACTIVE_HIGH>;
		tm1638-dio-gpios = <&arduino_header 7 GPIO_ACTIVE_HIGH>;
	};
};
```

Por llamarse exactamente `app.overlay` y estar en la raíz, Zephyr lo
aplica automáticamente sea cual sea la placa que compiles (es el
comportamiento documentado del sistema de build: primero busca
`boards/<BOARD>.overlay`, y solo si no existe usa `app.overlay`). No
hace falta tocar `CMakeLists.txt` ni pasar `-DDTC_OVERLAY_FILE`.

**Para usar otros pines físicos**: solo edita los tres números después
de `&arduino_header` en `app.overlay` (son los D0, D1, D2... del
header tipo Arduino del Nucleo). Si prefieres referenciar el
controlador GPIO directo en vez del header Arduino, cambia por ejemplo
`&arduino_header 2 GPIO_ACTIVE_HIGH` por `&gpioa 10 GPIO_ACTIVE_HIGH`
(pin PA10). `main.c` no necesita ningún cambio: sigue leyendo los
mismos 3 nombres de propiedad (`tm1638-stb-gpios`, etc.) sin importar a
qué pin físico apunten.

## 5. Compilar y flashear

```bash
west build -b nucleo_f401re tm1638_nucleo
west flash
```

## 6. Protocolo del TM1638 (cómo funciona por dentro)

- **3 líneas**: `STB` (chip select, activo en bajo durante cada
  transacción), `CLK` (reloj que generamos nosotros) y `DIO` (datos,
  **bidireccional**).
- **DIO bidireccional**: como GPIO genérico no puede ser entrada y
  salida a la vez, cada función de bajo nivel reconfigura el pin justo
  antes de usarlo: `gpio_pin_configure_dt(&tm.dio, GPIO_OUTPUT)` para
  transmitir, `GPIO_INPUT` para los 4 bytes que el TM1638 devuelve al
  leer el teclado. Esto pasa dentro de `send_byte()` / `recv_byte()`
  en `tm1638.c`, nunca lo tienes que manejar desde `main.c`.
- **Escritura**: `STB` en bajo, se manda `0x44` (modo dirección fija),
  `STB` en alto; luego `STB` en bajo otra vez, se manda `0xC0|dirección`
  seguido del byte de datos, `STB` en alto. Cada dirección (0-15) es un
  byte de la memoria de display: la dirección par `i*2` es el dígito
  `i`, la impar `i*2+1` es su LED asociado (así están cableados los
  módulos "LED&KEY" estándar, los más comunes con 8 dígitos + 8 LEDs +
  8 botones en fila).
- **Brillo**: un solo byte `0x88 | nivel` (bit 3 = pantalla encendida,
  bits 0-2 = brillo 0-7), enviado con su propio `STB` bajo/alto.
- **Lectura de botones**: `STB` en bajo, se manda `0x42`, se cambia
  `DIO` a entrada y se leen 4 bytes (cada uno trae el estado de 2
  botones, en el bit 0 y el bit 4), `STB` en alto, `DIO` vuelve a
  salida para la próxima escritura.

## 7. Mapeo de segmentos

`tm1638_set_digit(digit, segments)` usa el orden de bits que el propio
TM1638 espera en su registro de display (no hubo que reordenar nada):

```
bit 0 = a      bit 4 = e
bit 1 = b      bit 5 = f
bit 2 = c      bit 6 = g
bit 3 = d      bit 7 = dp (punto decimal)
```

`0xFF` → los 7 segmentos + el punto decimal encendidos.
`0x00` → todos los segmentos apagados.

## 8. Debounce de botones: cómo funciona

`tm1638_get_button()` implementa un antirrebote **no bloqueante**
(nunca llama a `k_sleep()` dentro de sí misma), basado en
`k_uptime_get()`:

1. Cada vez que se llama, lee el estado crudo de los 8 botones.
2. Si ese estado crudo cambió respecto a la lectura anterior, guarda
   el nuevo valor y reinicia un cronómetro (`keys_change_ts`) — pero
   **todavía no lo acepta** como válido, porque podría ser rebote
   mecánico.
3. Solo cuando el estado crudo lleva **30 ms sin volver a cambiar**
   (`TM1638_DEBOUNCE_MS`), se copia a `keys_stable`, que es lo que la
   función realmente reporta.

Por eso hay que llamarla periódicamente (en el `main.c` de ejemplo,
cada 20 ms dentro del `while (1)`): el "asentado" del antirrebote pasa
entre llamadas, no dentro de una sola llamada. Esto evita bloquear el
sistema con esperas largas — la CPU queda libre para el scheduler
durante el `k_msleep(20)` del loop, no hay ningún busy-wait.

**Varios botones a la vez**: como la función solo puede devolver un
único entero, si hay más de un botón presionado en el mismo instante
se prioriza el de menor número (el más a la izquierda). Si tu proyecto
necesita el estado de los 8 botones simultáneamente (por ejemplo para
combinaciones de teclas), la forma más simple es exponer también el
byte crudo `keys_stable` como una función adicional (`tm1638.c` ya
lleva ese bitmask calculado internamente) en vez de traducirlo a un
solo número.

## 9. Cambiar el brillo

`tm1638_init()` deja el brillo en nivel 4 por defecto. Para cambiarlo
en cualquier momento, llama a `tm1638_set_brightness(nivel)` con
`nivel` de 0 (mínimo) a 7 (máximo); un valor fuera de ese rango
devuelve `-EINVAL`.

## 10. Limitación a tener en cuenta

La librería guarda su estado en una variable estática dentro de
`tm1638.c`, así que está pensada para **un solo TM1638 conectado a la
vez** — es lo que permite llamar a `tm1638_display(numero)` sin pasar
ningún puntero de dispositivo. Si en algún momento necesitas manejar
dos módulos TM1638 al mismo tiempo, la librería habría que rediseñarla
para que cada función reciba un `struct` con el estado del dispositivo
en cuestión.
