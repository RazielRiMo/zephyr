/*
 * Copyright (c) 2012-2014 Wind River Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <zephyr/random/random.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/sys/util.h>
#include <zephyr/device.h>
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

static const struct adc_dt_spec vol =
	ADC_DT_SPEC_GET_BY_NAME(DT_PATH(zephyr_user), temp);

int last_button = 0;
int counter = 0;
int bot = 0;
volatile bool flag = true, menuflag = false;

uint64_t inicio, fin, tiempo;
uint32_t tiempo_aleatorio;

#define STACK_SIZE 512

#define PRIORITY_MAIN 5
#define PRIORITY_JUEGO 4
#define PRIORITY_BOTONES 3
#define PRIORITY_TIEMPO 2
#define PRIORITY_AUTOFAN 1
#define PRIORITY_READVOL 6

K_SEM_DEFINE (leer, 0, 1);
K_SEM_DEFINE (display, 0, 1);
K_SEM_DEFINE (autof, 0, 1);
K_SEM_DEFINE (juego, 0, 1);
K_SEM_DEFINE (stopleer, 0, 1);
K_SEM_DEFINE (stopdis, 0, 1);
K_SEM_DEFINE (stopauto, 0, 1);
K_SEM_DEFINE (stopjuego, 0, 1);
K_SEM_DEFINE (readvolt, 0, 1);
K_SEM_DEFINE (stopvolt, 0, 1);

K_MUTEX_DEFINE(acceso_hardware);



void menu (){
		printk("Menu de opciones:\n");
		printk("1) Ajustar brillo (0-7)\n");
		printk("2) remostrar mensaje de bienvenida\n");
		printk("3) leer botones y mostrar en display\n");
		printk("4) juego de reaccion con botones y leds\n");
}

void ajustar_brillo(){
	menuflag = true;
	int level = 0;
	k_mutex_lock(&acceso_hardware, K_FOREVER);
	tm1638_clear_digits();
	tm1638_set_digit(0, 0x73); //P o 0xce
	tm1638_set_digit(1, 0x77); //R o 0xee
	tm1638_set_digit(2, 0x79); //E o 0x9e
	tm1638_set_digit(3, 0x6D); //S o 0xb6
	tm1638_set_digit(4, 0xED); //S. o oxb7
	k_mutex_unlock(&acceso_hardware);
	while (menuflag){ 
		k_mutex_lock(&acceso_hardware, K_FOREVER);
		level = tm1638_get_button();
		k_mutex_unlock(&acceso_hardware);
		/* Solo reaccionamos en el flanco de "recien presionado",
		 * no en cada iteracion mientras se mantiene presionado.
		 */
		if (level > 0 && level != last_button) {

			k_mutex_lock(&acceso_hardware, K_FOREVER);
			tm1638_set_brightness(level-1);
			tm1638_clear_digits();
			tm1638_display(level-1);
			k_mutex_unlock(&acceso_hardware);

			printk("brillo ajustado a %d\n", level-1);
			if ((level-1)!=0) menuflag = false;
			}
		
		last_button = level;

		k_msleep(20);
	}
}

void mostrar_bienvenida(){

	k_mutex_lock(&acceso_hardware, K_FOREVER);
	tm1638_clear_digits();
	tm1638_set_digit(0, 0x76); //H o 0x76
	tm1638_set_digit(1, 0x79); //E o 0x9e
	tm1638_set_digit(2, 0x1C); //L o 0x1c este esta invertido por si las moscas
	tm1638_set_digit(3, 0x38); //L o 0x38
	tm1638_set_digit(4, 0xBF); //O o 0xfc 
	k_mutex_unlock(&acceso_hardware);
}

void mostrar_boton(){
	menuflag = true;
	k_mutex_lock(&acceso_hardware, K_FOREVER);
	tm1638_clear_digits();
	tm1638_set_digit(0, 0x73); //P o 0xce
	tm1638_set_digit(1, 0x77); //R o 0xee
	tm1638_set_digit(2, 0x79); //E o 0x9e
	tm1638_set_digit(3, 0x6D); //S o 0xb6
	tm1638_set_digit(4, 0xED); //S. o oxb7
	tm1638_set_digit(5, 0x3F); //O
	tm1638_set_digit(6, 0xBE); //U.
	tm1638_set_digit(7, 0xFF); //8
	k_mutex_unlock(&acceso_hardware);

	while (menuflag){ 

		k_mutex_lock(&acceso_hardware, K_FOREVER);
		int button = tm1638_get_button();
		k_mutex_unlock(&acceso_hardware);
		/* Solo reaccionamos en el flanco de "recien presionado",
		 * no en cada iteracion mientras se mantiene presionado.
		 */
		if (button > 0 && button != last_button) {

			
			k_mutex_lock(&acceso_hardware, K_FOREVER);
			tm1638_clear_digits();
			tm1638_display(button);
			k_mutex_unlock(&acceso_hardware);

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

	while(1){

		k_sem_take(&leer ,K_FOREVER);

		while (1) {

			if(k_sem_take(&stopleer, K_NO_WAIT)==0) break;

			k_mutex_lock(&acceso_hardware, K_FOREVER);
			int boton = tm1638_get_button();
			k_mutex_unlock(&acceso_hardware);
			/* Solo reaccionamos en el flanco de "recien presionado",
			* no en cada iteracion mientras se mantiene presionado.
			*/
			if (boton > 0 && boton != last_button) {
				k_sem_give(&stopjuego);
				printk("Boton %d presionado\n", boton);
			}
			last_button = boton;
		
			k_msleep(22);
		}
	}
}

void actualizar_tiempo(void){
	while (1) {

		k_sem_take (&display, K_FOREVER);
		
		while(1){
			if (k_sem_take(&stopdis, K_NO_WAIT) == 0) break;
			k_mutex_lock(&acceso_hardware, K_FOREVER);
			tm1638_clear_digits();
			k_mutex_unlock(&acceso_hardware);
			fin = k_uptime_get();
			tiempo = (fin - inicio)/10;
			k_mutex_lock(&acceso_hardware, K_FOREVER);
			tm1638_display(tiempo);
			k_mutex_unlock(&acceso_hardware);
			k_msleep(10);
		}
	}
}

void iniciar_juego(void){

	while (1){
	
		k_sem_take(&juego, K_FOREVER);

		while (1){

			k_mutex_lock(&acceso_hardware, K_FOREVER);
			tm1638_clear();
			k_mutex_unlock(&acceso_hardware);
			printk("Juego de reaccion iniciado. Espera a que se encienda el LED...\n");
			k_mutex_lock(&acceso_hardware, K_FOREVER);
			tm1638_display(3);
			k_mutex_unlock(&acceso_hardware);
			k_msleep(1000);
			k_mutex_lock(&acceso_hardware, K_FOREVER);
			tm1638_clear();
			tm1638_display(2);
			k_mutex_unlock(&acceso_hardware);
			k_msleep(1000);
			k_mutex_lock(&acceso_hardware, K_FOREVER);
			tm1638_clear();
			tm1638_display(1);
			k_mutex_unlock(&acceso_hardware);
			k_msleep(1000);
			k_mutex_lock(&acceso_hardware, K_FOREVER);
			tm1638_clear();
			k_mutex_unlock(&acceso_hardware);

			tiempo_aleatorio = (rand() % 3000) +1000;
			k_msleep(tiempo_aleatorio);
			inicio = k_uptime_get();

			k_sem_give(&display);
			k_sem_give(&leer);
			k_sem_give(&autof);

			k_sem_take(&stopjuego, K_FOREVER);

			k_sem_give(&stopdis);
			k_sem_give(&stopleer);

			bool ganador = false;
			
			k_mutex_lock(&acceso_hardware, K_FOREVER);
			tm1638_clear_leds();
			k_mutex_unlock(&acceso_hardware);
			for (int i = 0; i < 10; i++)
			{
				k_mutex_lock(&acceso_hardware, K_FOREVER);
				if (ganador) tm1638_set_brightness(7);
				else tm1638_set_brightness(0);
				k_mutex_unlock(&acceso_hardware);
				ganador = !ganador;
				k_msleep(100);
			}

			k_sem_give(&stopauto);

		}
	}
}

void autofan(void){
	while(1){

		k_sem_take(&autof, K_FOREVER);

		while (1){

			if (k_sem_take(&stopauto, K_NO_WAIT) == 0) break;

			for (int i = 0; i < 8; i++){
				k_mutex_lock(&acceso_hardware, K_FOREVER);
				tm1638_clear_leds();
				tm1638_set_led(i, 1);
				k_mutex_unlock(&acceso_hardware);
				k_msleep(50);
			}
			for (int t = 7; t >= 0; t--){
				k_mutex_lock(&acceso_hardware, K_FOREVER);
				tm1638_clear_leds();
				tm1638_set_led(t, 1);
				k_mutex_unlock(&acceso_hardware);
				k_msleep(50);
			}
		}
	}
}

void readvol(void){
	int16_t rawvol;
	int32_t mv;
	
	int rec;

	int num;

	struct adc_sequence sec = {
    	.buffer = &rawvol,
    	.buffer_size = sizeof(rawvol),
    };
	k_sem_take(&readvolt, K_FOREVER);
	while (1){
		if (k_sem_take(&stopvolt, K_NO_WAIT) == 0) break;
		rec = adc_sequence_init_dt(&vol, &sec);
		if (rec!=0){
			printk("error al iniciar secuencia");
			break;
		}
		rec = adc_read(vol.dev, &sec);
		if (rec!=0){
			printk("error al leer");
			break;
		}
		num = rawvol*10000;
		mv = rawvol;

		rec = adc_raw_to_millivolts_dt(&vol, &mv);
		if (rec!=0){

			printk("error al iniciar transformar");
			break;
		}
		num = num + mv;
		
		k_mutex_lock(&acceso_hardware, K_FOREVER);
		tm1638_clear_digits();
		tm1638_display(num);
		k_mutex_unlock(&acceso_hardware);
		printk("Voltaje leido: %d mV\n", mv);
		
		k_msleep(500);
	}
}

void mainloop(void)
{
	while (1) {
		int ret;

		printk("Iniciando demo de TM1638...\n");
		/* 1) Inicializar el TM1638 */
		if (!device_is_ready(vol.dev)){
			printk("ADC no esta listo");
			break;
		}

		ret = adc_channel_setup_dt(&vol);

		if (ret != 0){
			printk("ADC no configurado");
		}

		k_mutex_lock(&acceso_hardware, K_FOREVER);
		printk("Antes de init\n");
		ret = tm1638_init(stb_pin, clk_pin, dio_pin);
		printk("Despues de init ret=%d\n", ret);
		k_mutex_unlock(&acceso_hardware);
		printk("ret definido\n");
		if (ret != 0) {
			printk("Error al inicializar el TM1638 (ret=%d)\n", ret);
			break;
		}
		printk("procesado\n");
		/* 2) tm1638_set_brightness(0-7): establecer el brillo */
		k_mutex_lock(&acceso_hardware, K_FOREVER);
		tm1638_set_brightness(7);
		printk("brillo establecido\n");
		/* 3) tm1638_display(): mostrar un numero en los 8 digitos */
		tm1638_display(12345678);
		printk("12345678\n");
		k_mutex_unlock(&acceso_hardware);
		k_msleep(1500);

		/* 4) tm1638_set_digit(): control manual de un digito. */
		/*    0x77 = segmentos a,b,c,e,f,g encendidos -> dibuja una "A". */
		k_mutex_lock(&acceso_hardware, K_FOREVER);
		tm1638_set_digit(2, 0x77);
		printk("set digit\n");
		k_mutex_unlock(&acceso_hardware);
		k_msleep(1000);

		/* 5) tm1638_set_digit(): control manual de un digito.
		*    0xFF enciende los 7 segmentos + el punto decimal del digito */
		k_mutex_lock(&acceso_hardware, K_FOREVER);
		tm1638_set_digit(0, 0xFF);
		k_mutex_unlock(&acceso_hardware);
		k_msleep(1000);

		/* 6) tm1638_set_led(): encender y apagar LEDs individuales */
		for (int i = 0; i < 8; i++) {
			k_mutex_lock(&acceso_hardware, K_FOREVER);
			tm1638_set_led(i, 1);
			k_mutex_unlock(&acceso_hardware);
			k_msleep(50);
		}
		printk("led on\n");
		k_msleep(500);
		for (int i = 0; i < 8; i++) {
			k_mutex_lock(&acceso_hardware, K_FOREVER);
			tm1638_set_led(i, 0);
			k_mutex_unlock(&acceso_hardware);
			k_msleep(50);
		}
		printk("ledof\n");
		/* 7) tm1638_clear_digits(): apaga solo los 7 segmentos, los LEDs
		*    quedan como esten (se nota porque dejamos uno encendido). */
		k_mutex_lock(&acceso_hardware, K_FOREVER);
		tm1638_set_led(3, 1);
		printk("setled\n");
		tm1638_display(8888);
		printk("setdisplay\n");
		k_mutex_unlock(&acceso_hardware);
		k_msleep(200);
		k_mutex_lock(&acceso_hardware, K_FOREVER);
		tm1638_clear_digits();
		k_mutex_unlock(&acceso_hardware);
		k_msleep(200);

		/* 8) tm1638_clear_leds(): apaga solo los LEDs */
		k_mutex_lock(&acceso_hardware, K_FOREVER);
		tm1638_clear_leds();
		printk("clearled\n");
		k_mutex_unlock(&acceso_hardware);
		k_msleep(500);

		/* 9) tm1638_clear(): apaga absolutamente todo */
		k_mutex_lock(&acceso_hardware, K_FOREVER);
		tm1638_display(1234);
		printk("1234\n");
		tm1638_set_led(5, 1);
		printk("ledon\n");
		k_mutex_unlock(&acceso_hardware);
		k_msleep(800);
		k_mutex_lock(&acceso_hardware, K_FOREVER);
		tm1638_clear();
		printk("clear all\n");
		k_mutex_unlock(&acceso_hardware);
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
			k_mutex_lock(&acceso_hardware, K_FOREVER);
			int button = tm1638_get_button();
			k_mutex_unlock(&acceso_hardware);

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
						k_sem_give(&juego);
						flag = false;
						break;
					case 5:
						printk("leer voltaje\n");
						k_sem_give(&readvolt);
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
K_THREAD_DEFINE (leer_vol, STACK_SIZE, readvol, NULL, NULL, NULL,
	PRIORITY_READVOL, 0, 0);
