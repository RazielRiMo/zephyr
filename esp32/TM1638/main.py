import asyncio#type: ignore
import random
import time
from tm1638 import TM1638

tm = TM1638(stb=4, clk=18, dio=19)

hw_lock = asyncio.Lock()#t  ype: ignore


ev_leer = asyncio.Event()#type: ignore
ev_display = asyncio.Event()#type: ignore
ev_autof = asyncio.Event()#type: ignore
ev_juego = asyncio.Event()#type: ignore
ev_stopleer = asyncio.Event()#type: ignore
ev_stopdis = asyncio.Event()#type: ignore
ev_stopauto = asyncio.Event()#type: ignore
ev_stopjuego = asyncio.Event()#type: ignore

last_button = 0
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
    await asyncio.sleep_ms(200)#type: ignore
    while menuflag:
        async with hw_lock:
            level = tm.get_button()

        if level > 0 and level != last_button:
            async with hw_lock:
                tm.set_brightness(level - 1)
                tm.clear_digits()
                tm.display(level - 1)

            print("brillo ajustado a", level - 1)
            if (level - 1) != 0:
                menuflag = False

        last_button = level
        await asyncio.sleep_ms(20)#type: ignore


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
                await asyncio.sleep_ms(2000)#type: ignore

        last_button = button
        await asyncio.sleep_ms(20)#type: ignore


async def leer_botones():
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
            await asyncio.sleep_ms(22)#type: ignore


async def actualizar_tiempo():
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

            fin = time.ticks_ms()#type: ignore
            tiempo = time.ticks_diff(fin, inicio) // 10#type: ignore

            async with hw_lock:
                tm.display(tiempo)

            await asyncio.sleep_ms(10)#type: ignore


async def iniciar_juego():
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
            await asyncio.sleep_ms(1000)#type: ignore

            async with hw_lock:
                tm.clear()
                tm.display(2)
            await asyncio.sleep_ms(1000)#type: ignore

            async with hw_lock:
                tm.clear()
                tm.display(1)
            await asyncio.sleep_ms(1000)#type: ignore

            async with hw_lock:
                tm.clear()

            tiempo_aleatorio = random.randint(1000, 3999)
            await asyncio.sleep_ms(tiempo_aleatorio)#type: ignore
            inicio = time.ticks_ms()#type: ignore

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
                await asyncio.sleep_ms(100)#type: ignore

            ev_stopauto.set()


async def autofan():
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
                await asyncio.sleep_ms(50)#type: ignore

            for t in range(7, -1, -1):
                async with hw_lock:
                    tm.clear_leds()
                    tm.set_led(t, 1)
                await asyncio.sleep_ms(50)#type: ignore


async def mainloop():

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
    await asyncio.sleep_ms(1500)#type: ignore

    # 4) tm1638_set_digit(): 0x77 = segmentos a,b,c,e,f,g -> "A"
    async with hw_lock:
        tm.set_digit(2, 0x77)
    await asyncio.sleep_ms(1000)#type: ignore

    # 5) 0xFF enciende los 7 segmentos + el punto decimal
    async with hw_lock:
        tm.set_digit(0, 0xFF)
    await asyncio.sleep_ms(1000)#type: ignore

    # 6) tm1638_set_led(): encender y apagar LEDs individuales
    for i in range(8):
        async with hw_lock:
            tm.set_led(i, 1)
        await asyncio.sleep_ms(100)#type: ignore
    await asyncio.sleep_ms(500)#type: ignore
    for i in range(8):
        async with hw_lock:
            tm.set_led(i, 0)
        await asyncio.sleep_ms(100)#type: ignore

    # 7) tm1638_clear_digits(): apaga solo los 7 segmentos
    async with hw_lock:
        tm.set_led(3, 1)
        tm.display(8888)
    await asyncio.sleep_ms(800)#type: ignore
    async with hw_lock:
        tm.clear_digits()
    await asyncio.sleep_ms(800)#type: ignore

    # 8) tm1638_clear_leds(): apaga solo los LEDs
    async with hw_lock:
        tm.clear_leds()
    await asyncio.sleep_ms(500)#type: ignore

    # 9) tm1638_clear(): apaga absolutamente todo
    async with hw_lock:
        tm.display(1234)
        tm.set_led(5, 1)
    await asyncio.sleep_ms(800)#type: ignore
    async with hw_lock:
        tm.clear()
    await asyncio.sleep_ms(500)#type: ignore

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
                await asyncio.sleep_ms(2000)#type: ignore
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
        await asyncio.sleep_ms(20)#type: ignore

    while True:
        await asyncio.sleep(3600)#type: ignore


async def main():

    asyncio.create_task(leer_botones())#type: ignore
    asyncio.create_task(actualizar_tiempo())#type: ignore
    asyncio.create_task(autofan())#type: ignore
    asyncio.create_task(iniciar_juego())#type: ignore

    await mainloop()


asyncio.run(main())#type: ignore
