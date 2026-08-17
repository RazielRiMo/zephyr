/*
 * Copyright (c) 2012-2014 Wind River Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <zephyr/random/random.h>
#include "tm1638.h"
#include <stdlib.h>

/*
 * Los 3 pines se definen en app.overlay dentro del nodo estandar
 * "zephyr,user" -- el patron recomendado por Zephyr para exponer GPIO
 * sueltos a la aplicacion sin escribir un devicetree binding propio.
 * Si cambias de pines fisicos, solo hay que editar app.overlay: este
 * archivo no cambia.
 */
static const struct gpio_dt_spec stb_pin =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), tm1638_stb_gpios);
static const struct gpio_dt_spec clk_pin =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), tm1638_clk_gpios);
static const struct gpio_dt_spec dio_pin =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), tm1638_dio_gpios);

int last_button = 0;
int counter = 0;
int bot = 0;
bool flag = true, menuflag = true, game = false, stop = false, autofantastico = false, maint=true;

uint64_t inicio, fin, tiempo;
uint32_t tiempo_aleatorio;

#define STACK_SIZE 512

#define PRIORITY_MAIN 5
#define PRIORITY_JUEGO 4
#define PRIORITY_BOTONES 3
#define PRIORITY_TIEMPO 2
#define PRIORITY_AUTOFAN 1



void menu (void){
		printk("Menu de opciones:\n");
		printk("1) Ajustar brillo (0-7)\n");
		printk("2) remostrar mensaje de bienvenida\n");
		printk("3) leer botones y mostrar en display\n");
		printk("4) juego de reaccion con botones y leds\n");
}

void ajustar_brillo(){
	menuflag = true;
	int level = 0;
	tm1638_clear_digits();
	tm1638_set_digit(0, 0xCE); //P
	tm1638_set_digit(1, 0xEE); //R
	tm1638_set_digit(2, 0x9E); //E
	tm1638_set_digit(3, 0xB6); //S
	tm1638_set_digit(4, 0xB7); //S.
	while (menuflag){ 
		level = tm1638_get_button();

		/* Solo reaccionamos en el flanco de "recien presionado",
		 * no en cada iteracion mientras se mantiene presionado.
		 */
		if (level > 0 && level != last_button) {
			tm1638_set_brightness(level-1);
			tm1638_clear_digits();
			tm1638_display(level-1);
			printk("brillo ajustado a %d\n", level-1);
			menuflag = false;
			}
		
		last_button = level;

		k_msleep(20);
	}
}

void mostrar_bienvenida(){
	tm1638_clear_digits();
	tm1638_set_digit(0, 0x76); //H
	tm1638_set_digit(1, 0x9E); //E
	tm1638_set_digit(2, 0x1C); //L
	tm1638_set_digit(3, 0x38); //L
	tm1638_set_digit(4, 0xFC); //O
}

void mostrar_boton(){
	menuflag = true;

	tm1638_clear_digits();
	tm1638_set_digit(0, 0xCE); //P
	tm1638_set_digit(1, 0xEE); //R
	tm1638_set_digit(2, 0x9E); //E
	tm1638_set_digit(3, 0xB6); //S
	tm1638_set_digit(4, 0xB7); //S.
	tm1638_set_digit(5, 0xFC); //O
	tm1638_set_digit(6, 0x7C); //U
	tm1638_set_digit(7, 0xFF); //8

	while (menuflag){ 
		int button = tm1638_get_button();

		/* Solo reaccionamos en el flanco de "recien presionado",
		 * no en cada iteracion mientras se mantiene presionado.
		 */
		if (button > 0 && button != last_button) {
			tm1638_clear_digits();
			tm1638_display(button);
			printk("Boton %d presionado\n", button);
			if (button == 8){
				menuflag = false;
				last_button = button;
				printk("Saliendo de la funcion mostrar_boton\n");
				k_msleep(2000);
			}
		}
		last_button = button;

		k_msleep(20);
	}
}

void leer_botones(void){
	while (1) {
		if (game && !maint){
			int boton = tm1638_get_button();

			/* Solo reaccionamos en el flanco de "recien presionado",
			 * no en cada iteracion mientras se mantiene presionado.
			 */
			if (boton > 0 && boton != last_button) {
				stop = true;
				printk("Boton %d presionado\n", boton);
			}
			last_button = boton;
		}
		k_msleep(22);}
}

void actualizar_tiempo(void){
	while (1) {
		if (game && !maint){
			tm1638_clear_digits();
			fin = k_uptime_get();
			tiempo = (fin - inicio)/10;
			tm1638_display(tiempo);
		}
		k_msleep(10);
	}
}

void iniciar_juego(void){
	tm1638_clear();
	if (!game && !maint){
		tm1638_clear();
		printk("Juego de reaccion iniciado. Espera a que se encienda el LED...\n");
		tm1638_display(3);
		k_msleep(1000);
		tm1638_clear();
		tm1638_display(2);
		k_msleep(1000);
		tm1638_clear();
		tm1638_display(1);
		k_msleep(1000);
		tm1638_clear();

		tiempo_aleatorio = (rand() % 3000) +1000;
		k_msleep(tiempo_aleatorio);
		inicio = k_uptime_get();
		game = true;
		autofantastico = true;
		stop = false;
		while (!stop){
			k_msleep(3);
		}
		autofantastico = false;
		game = false;
		bool ganador = false;
		tm1638_clear_leds();
		for (int i = 0; i < 10; i++)
		{
			if (ganador) tm1638_set_brightness(7);
			else tm1638_set_brightness(0);
			ganador = !ganador;
			k_msleep(100);
		}
	}
}

void autofan(void){
	while(1){
		if (autofantastico && !maint){
			for (int i = 0; i < 8; i++){
				tm1638_clear_leds();
				tm1638_set_led(i, 1);
				k_msleep(50);
			}
			for (int t = 7; t >= 0; t--){
				tm1638_clear_leds();
				tm1638_set_led(t, 1);
				k_msleep(50);
			}
		}
		else k_msleep(12);
	}
}

void mainloop(void)
{
	while (1) {
		int ret;

		/* 1) Inicializar el TM1638 */
		ret = tm1638_init(stb_pin, clk_pin, dio_pin);
		if (ret != 0) {
			printk("Error al inicializar el TM1638 (ret=%d)\n", ret);
			return;
		}
		
		/* 2) tm1638_set_brightness(0-7): establecer el brillo */
		tm1638_set_brightness(7);

		/* 3) tm1638_display(): mostrar un numero en los 8 digitos */
		tm1638_display(12345678);
		k_msleep(1500);

		/* 4) tm1638_set_digit(): control manual de un digito.
		*    0x77 = segmentos a,b,c,e,f,g encendidos -> dibuja una "A". */
		tm1638_set_digit(2, 0x77);
		k_msleep(1000);

		/* 5) tm1638_set_digit(): control manual de un digito.
		*    0xFF enciende los 7 segmentos + el punto decimal del digito */
		tm1638_set_digit(0, 0xFF);
		k_msleep(1000);

		/* 6) tm1638_set_led(): encender y apagar LEDs individuales */
		for (int i = 0; i < 8; i++) {
			tm1638_set_led(i, 1);
			k_msleep(100);
		}
		k_msleep(500);
		for (int i = 0; i < 8; i++) {
			tm1638_set_led(i, 0);
			k_msleep(100);
		}

		/* 7) tm1638_clear_digits(): apaga solo los 7 segmentos, los LEDs
		*    quedan como esten (se nota porque dejamos uno encendido). */
		tm1638_set_led(3, 1);
		tm1638_display(8888);
		k_msleep(800);
		tm1638_clear_digits();
		k_msleep(800);

		/* 8) tm1638_clear_leds(): apaga solo los LEDs */
		tm1638_clear_leds();
		k_msleep(500);

		/* 9) tm1638_clear(): apaga absolutamente todo */
		tm1638_display(1234);
		tm1638_set_led(5, 1);
		k_msleep(800);
		tm1638_clear();
		k_msleep(500);

		printk("Demo inicial terminada. Presiona los botones del modulo...\n");

		/*
		* 10), 11) y 12): loop principal.
		*
		* tm1638_get_button() ya aplica antirrebote internamente, pero
		* necesita que se le llame de forma periodica para que ese
		* antirrebote pueda "asentarse" (ver tm1638.c). Sondeamos cada
		* 20 ms con k_msleep(), que cede la CPU al scheduler en vez de
		* hacer busy-wait -- el nucleo puede atender otros hilos o entrar
		* en idle durante esa espera, no se desperdicia CPU.
		*/
		menu();

		while (flag) {
			int button = tm1638_get_button();

			/* Solo reaccionamos en el flanco de "recien presionado",
			* no en cada iteracion mientras se mantiene presionado.
			*/
			if (button > 0 && button != last_button) {
				switch (button) {
					case 1:
						ajustar_brillo();
						menu();
						break;
					case 2:
						mostrar_bienvenida();
						k_msleep(2000);
						menu();
						break;
					case 3:
						mostrar_boton();
						menu();
						break;
					case 4:
						flag = false;
						break;
					default:
						printk("Opcion invalida\n");
						menu();
						break;
				}
			}
			last_button = button;

			k_msleep(20);
		}
	maint = false;
	k_sleep(K_FOREVER);
}
}
K_THREAD_DEFINE (main_id, STACK_SIZE, mainloop, NULL, NULL, NULL,
	PRIORITY_MAIN, 0, 0);
K_THREAD_DEFINE (botones_id, STACK_SIZE, leer_botones, NULL, NULL, NULL,
	PRIORITY_BOTONES, 0, 0);
K_THREAD_DEFINE (tiempo_id, STACK_SIZE, actualizar_tiempo, NULL, NULL, NULL,
	PRIORITY_TIEMPO, 0, 0);
K_THREAD_DEFINE (autofan_id, STACK_SIZE, autofan, NULL, NULL, NULL,
	PRIORITY_AUTOFAN, 0, 0);
K_THREAD_DEFINE (juego_id, STACK_SIZE, iniciar_juego, NULL, NULL, NULL,
	PRIORITY_JUEGO, 0, 0);
