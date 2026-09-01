"""
Traduccion a MicroPython (ESP32) de main.c: menu interactivo por
botones del TM1638 + juego de reaccion con LEDs y temporizador.

Zephyr usa hilos con prioridad (K_THREAD_DEFINE) y semaforos/mutex del
kernel para coordinarlos. MicroPython no tiene ese modelo, asi que aqui
se usa asyncio (concurrencia COOPERATIVA, no apropiativa -- no hay
prioridades reales, cada tarea cede el control en cada `await`):

    Zephyr                          asyncio
    ------------------------------  ------------------------------
    K_THREAD_DEFINE(fn, prio, ...)  asyncio.create_task(fn())
    K_SEM_DEFINE(sem, 0, 1)         ev = asyncio.Event()
    k_sem_give(&sem)                ev.set()
    k_sem_take(&sem, K_FOREVER)     await ev.wait(); ev.clear()
    k_sem_take(&sem, K_NO_WAIT)==0  ev.is_set()  (y si es True: ev.clear())
    K_MUTEX_DEFINE(m)               lock = asyncio.Lock()
    k_mutex_lock(&m, K_FOREVER)     async with lock:
    k_mutex_unlock(&m)                  ...
    k_msleep(n)                     await asyncio.sleep_ms(n)
    k_uptime_get()                  time.ticks_ms()  (+ time.ticks_diff)

Dos correcciones respecto al main.c original (explicadas en el chat):
1. En el C original, los pasos 2-9 de la demo inicial quedaban dentro
   de un comentario /* ... */ sin cerrar y nunca se ejecutaban. Aqui
   SI se ejecutan, asumiendo que esa era la intencion.
2. actualizar_tiempo() comprobaba stopdis sin el "== 0" (logica
   invertida respecto a leer_botones/autofan). Aqui esta corregido.
"""

import asyncio
import random
import time
from tm1638 import TM1638

# ---------------------------------------------------------------
# Hardware
# ---------------------------------------------------------------
tm = TM1638(stb=4, clk=18, dio=19)

# Mutex de Zephyr -> asyncio.Lock, usado como "async with hw_lock:"
hw_lock = asyncio.Lock()

# Semaforos binarios de Zephyr -> asyncio.Event
ev_leer = asyncio.Event()
ev_display = asyncio.Event()
ev_autof = asyncio.Event()
ev_juego = asyncio.Event()
ev_stopleer = asyncio.Event()
ev_stopdis = asyncio.Event()
ev_stopauto = asyncio.Event()
ev_stopjuego = asyncio.Event()

# ---------------------------------------------------------------
# Estado compartido (igual que las variables globales del main.c)
# ---------------------------------------------------------------
last_button = 0
counter = 0   # sin uso -- se conserva por fidelidad con el C original
bot = 0       # sin uso -- idem
flag = True
menuflag = False

inicio = 0
fin = 0
tiempo = 0
tiempo_aleatorio = 0


def menu():
    print("Menu de opciones:")
    print("1) Ajustar brillo (0-7)")
    print("2) remostrar mensaje de bienvenida")
    print("3) leer botones y mostrar en display")
    print("4) juego de reaccion con botones y leds")


async def ajustar_brillo():
    global menuflag, last_button

    menuflag = True
    async with hw_lock:
        tm.clear_digits()
        tm.set_digit(0, 0x73)  # P
        tm.set_digit(1, 0x77)  # R
        tm.set_digit(2, 0x79)  # E
        tm.set_digit(3, 0x6D)  # S
        tm.set_digit(4, 0xED)  # S.

    while menuflag:
        async with hw_lock:
            level = tm.get_button()

        # Solo reaccionamos en el flanco de "recien presionado".
        if level > 0 and level != last_button:
            async with hw_lock:
                tm.set_brightness(level - 1)
                tm.clear_digits()
                tm.display(level - 1)

            print("brillo ajustado a", level - 1)
            if (level - 1) != 0:
                menuflag = False

        last_button = level
        await asyncio.sleep_ms(20)


async def mostrar_bienvenida():
    async with hw_lock:
        tm.clear_digits()
        tm.set_digit(0, 0x76)  # H
        tm.set_digit(1, 0x79)  # E
        tm.set_digit(2, 0x1C)  # L (invertido a proposito)
        tm.set_digit(3, 0x38)  # L
        tm.set_digit(4, 0xBF)  # O


async def mostrar_boton():
    global menuflag, last_button

    menuflag = True
    async with hw_lock:
        tm.clear_digits()
        tm.set_digit(0, 0x73)  # P
        tm.set_digit(1, 0x77)  # R
        tm.set_digit(2, 0x79)  # E
        tm.set_digit(3, 0x6D)  # S
        tm.set_digit(4, 0xED)  # S.
        tm.set_digit(5, 0x3F)  # O
        tm.set_digit(6, 0xBE)  # U.
        tm.set_digit(7, 0xFF)  # 8

    while menuflag:
        async with hw_lock:
            button = tm.get_button()

        if button > 0 and button != last_button:
            async with hw_lock:
                tm.clear_digits()
                tm.display(button)

            print("Boton", button, "presionado")
            if button == 8:
                menuflag = False
                last_button = button
                print("Saliendo de la funcion mostrar_boton")
                await asyncio.sleep_ms(2000)

        last_button = button
        await asyncio.sleep_ms(20)


async def leer_botones():
    """Equivalente a la tarea leer_botones() del main.c."""
    global last_button

    while True:
        await ev_leer.wait()
        ev_leer.clear()

        while True:
            if ev_stopleer.is_set():
                ev_stopleer.clear()
                break

            async with hw_lock:
                boton = tm.get_button()

            if boton > 0 and boton != last_button:
                ev_stopjuego.set()
                print("Boton", boton, "presionado")

            last_button = boton
            await asyncio.sleep_ms(22)


async def actualizar_tiempo():
    """
    Equivalente a actualizar_tiempo() del main.c, con la comprobacion
    de stopdis corregida (ver nota al inicio del archivo).
    """
    global fin, tiempo

    while True:
        await ev_display.wait()
        ev_display.clear()

        while True:
            if ev_stopdis.is_set():
                ev_stopdis.clear()
                break

            async with hw_lock:
                tm.clear_digits()

            fin = time.ticks_ms()
            tiempo = time.ticks_diff(fin, inicio) // 10

            async with hw_lock:
                tm.display(tiempo)

            await asyncio.sleep_ms(10)


async def iniciar_juego():
    """Equivalente a iniciar_juego() del main.c."""
    global inicio, tiempo_aleatorio

    while True:
        await ev_juego.wait()
        ev_juego.clear()

        while True:
            async with hw_lock:
                tm.clear()
            print("Juego de reaccion iniciado. Espera a que se encienda el LED...")

            async with hw_lock:
                tm.display(3)
            await asyncio.sleep_ms(1000)

            async with hw_lock:
                tm.clear()
                tm.display(2)
            await asyncio.sleep_ms(1000)

            async with hw_lock:
                tm.clear()
                tm.display(1)
            await asyncio.sleep_ms(1000)

            async with hw_lock:
                tm.clear()

            tiempo_aleatorio = random.randint(1000, 3999)
            await asyncio.sleep_ms(tiempo_aleatorio)
            inicio = time.ticks_ms()

            ev_display.set()
            ev_leer.set()
            ev_autof.set()

            await ev_stopjuego.wait()
            ev_stopjuego.clear()

            ev_stopdis.set()
            ev_stopleer.set()

            ganador = False

            async with hw_lock:
                tm.clear_leds()

            for _ in range(10):
                async with hw_lock:
                    tm.set_brightness(7 if ganador else 0)
                ganador = not ganador
                await asyncio.sleep_ms(100)

            ev_stopauto.set()


async def autofan():
    """Equivalente a autofan() del main.c."""
    while True:
        await ev_autof.wait()
        ev_autof.clear()

        while True:
            if ev_stopauto.is_set():
                ev_stopauto.clear()
                break

            for i in range(8):
                async with hw_lock:
                    tm.clear_leds()
                    tm.set_led(i, 1)
                await asyncio.sleep_ms(50)

            for t in range(7, -1, -1):
                async with hw_lock:
                    tm.clear_leds()
                    tm.set_led(t, 1)
                await asyncio.sleep_ms(50)


async def mainloop():
    """
    Equivalente a mainloop() del main.c. El while(1) exterior del
    original no se repite nunca en la practica (termina en un sleep
    infinito), asi que aqui se escribe en linea recta.
    """
    global last_button, flag

    # 1) Inicializar el TM1638: ya se hizo al crear `tm` al principio
    #    del archivo (constructor TM1638(...)).

    # 2) tm1638_set_brightness(0-7) + 3) tm1638_display(): demo inicial.
    #    (En el main.c original estos pasos quedaban dentro de un
    #    comentario sin cerrar y nunca se ejecutaban -- ver nota al
    #    inicio del archivo.)
    async with hw_lock:
        tm.set_brightness(7)
        tm.display(12345678)
    await asyncio.sleep_ms(1500)

    # 4) tm1638_set_digit(): 0x77 = segmentos a,b,c,e,f,g -> "A"
    async with hw_lock:
        tm.set_digit(2, 0x77)
    await asyncio.sleep_ms(1000)

    # 5) 0xFF enciende los 7 segmentos + el punto decimal
    async with hw_lock:
        tm.set_digit(0, 0xFF)
    await asyncio.sleep_ms(1000)

    # 6) tm1638_set_led(): encender y apagar LEDs individuales
    for i in range(8):
        async with hw_lock:
            tm.set_led(i, 1)
        await asyncio.sleep_ms(100)
    await asyncio.sleep_ms(500)
    for i in range(8):
        async with hw_lock:
            tm.set_led(i, 0)
        await asyncio.sleep_ms(100)

    # 7) tm1638_clear_digits(): apaga solo los 7 segmentos
    async with hw_lock:
        tm.set_led(3, 1)
        tm.display(8888)
    await asyncio.sleep_ms(800)
    async with hw_lock:
        tm.clear_digits()
    await asyncio.sleep_ms(800)

    # 8) tm1638_clear_leds(): apaga solo los LEDs
    async with hw_lock:
        tm.clear_leds()
    await asyncio.sleep_ms(500)

    # 9) tm1638_clear(): apaga absolutamente todo
    async with hw_lock:
        tm.display(1234)
        tm.set_led(5, 1)
    await asyncio.sleep_ms(800)
    async with hw_lock:
        tm.clear()
    await asyncio.sleep_ms(500)

    print("Demo inicial terminada. Presiona los botones del modulo...")

    # 10), 11) y 12): loop principal del menu.
    menu()

    while flag:
        async with hw_lock:
            button = tm.get_button()

        if button > 0 and button != last_button:
            if button == 1:
                await ajustar_brillo()
                menu()
            elif button == 2:
                await mostrar_bienvenida()
                await asyncio.sleep_ms(2000)
                menu()
            elif button == 3:
                await mostrar_boton()
                menu()
            elif button == 4:
                ev_juego.set()
                flag = False
            else:
                print("Opcion invalida")
                menu()

        last_button = button
        await asyncio.sleep_ms(20)

    # Equivalente a k_sleep(K_FOREVER): esta tarea ya no hace nada mas,
    # el juego sigue vivo en las otras tareas.
    while True:
        await asyncio.sleep(3600)


async def main():
    # Equivalente a los K_THREAD_DEFINE del main.c. El orden aqui no
    # implica prioridad (asyncio no tiene prioridades) -- cada tarea
    # se queda esperando su Event hasta que algo la despierte.
    asyncio.create_task(leer_botones())
    asyncio.create_task(actualizar_tiempo())
    asyncio.create_task(autofan())
    asyncio.create_task(iniciar_juego())

    await mainloop()


asyncio.run(main())
