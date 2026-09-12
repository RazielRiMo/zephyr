/*
 * Copyright (c) 2012-2014 Wind River Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/sys/printk.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/sys/util.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include "tm1638.h"	
#include <stdlib.h>
#include <math.h>

LOG_MODULE_REGISTER(sensores, LOG_LEVEL_DBG);

static const struct gpio_dt_spec stb_pin =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), tm1638_stb_gpios);
static const struct gpio_dt_spec clk_pin =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), tm1638_clk_gpios);
static const struct gpio_dt_spec dio_pin =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), tm1638_dio_gpios);

static const struct adc_dt_spec tem =
	ADC_DT_SPEC_GET_BY_NAME(DT_PATH(zephyr_user), temp);

static const struct adc_dt_spec lum =
	ADC_DT_SPEC_GET_BY_NAME(DT_PATH(zephyr_user), lumens);

#define RTC_NODE DT_NODELABEL(rtc)

#define STACK_SIZE 512

#define PRIORITY_MAIN 1
#define PRIORITY_READ_ADC 4
#define PRIORITY_PROM_ADC 4
#define PRIORITY_DISPLAY 3
#define PRIORITY_HORA 2
#define PRIORITY_READ_TM1638 5

K_MUTEX_DEFINE(hw_access);

K_SEM_DEFINE(botread, 0,1);
K_SEM_DEFINE(display,0,1);
K_SEM_DEFINE(rawr,0,1);
K_SEM_DEFINE(finer,0,1);
K_SEM_DEFINE(rlums,0,1);
K_SEM_DEFINE(rtemp,0,1);
K_SEM_DEFINE(conv,0,1);
K_SEM_DEFINE(hora,0,1);

K_SEM_DEFINE(stopbot, 0,1);
K_SEM_DEFINE(stoprawr,0,1);
K_SEM_DEFINE(stopdis, 0,1);
K_SEM_DEFINE(stoptemp,0,1);
K_SEM_DEFINE(stoplums,0,1);
K_SEM_DEFINE(stopconv,0,1);
K_SEM_DEFINE(horastop,0,1);

struct lectura_fina {
	uint16_t voltem;
	uint16_t vollum;
	struct rtc_time horaa;
};

struct lectura_raw {
	uint16_t volte;
	uint16_t vollu;
};

K_MSGQ_DEFINE(hora_act, sizeof(struct rtc_time),1 , 1);
K_MSGQ_DEFINE(lec_raw , sizeof(struct lectura_raw),10, 1);
K_MSGQ_DEFINE(lec_fin , sizeof(struct lectura_fina), 1, 1);
K_MSGQ_DEFINE(lec_disp, sizeof(int), 1, 1);

static const struct device *rtc = DEVICE_DT_GET(RTC_NODE);


void mainloop(void){
	int rec;
	if (!device_is_ready(tem.dev)){
		LOG_WRN("ADC no esta listo");
	}
	rec = adc_channel_setup_dt(&tem);
	if (rec!=0){
		LOG_WRN("ADC no se pudo configurar");
	}
	if (!device_is_ready(lum.dev)){
		LOG_WRN("ADC no esta listo");
	}
	rec = adc_channel_setup_dt(&lum);
	if (rec!=0){
		LOG_WRN("ADC no se pudo configurar");
	}

	k_sem_give(&rawr);

	k_mutex_lock(&hw_access, K_FOREVER);
	rec = tm1638_init(stb_pin, clk_pin, dio_pin);
	k_mutex_unlock(&hw_access);
	if(rec!=0){
		LOG_WRN("error al iniciar el tm1638 rec = %d\n", rec);
	}
	k_mutex_lock(&hw_access, K_FOREVER);
	tm1638_set_brightness(7);
	tm1638_clear();
	tm1638_set_digit(0, 0x73); //P o 0xce
	tm1638_set_digit(1, 0x77); //R o 0xee
	tm1638_set_digit(2, 0x79); //E o 0x9e
	tm1638_set_digit(3, 0x6D); //S o 0xb6
	tm1638_set_digit(4, 0xED); //S. o oxb7
	k_mutex_unlock(&hw_access);
	k_sem_give(&botread);
	k_sleep(K_FOREVER);
}

void leer_botones(void){
	int last_button = 0, boton;

	while(1){

		k_sem_take(&botread ,K_FOREVER);

		
		while (1) {

			if(k_sem_take(&stopbot, K_NO_WAIT)==0) break;

			k_mutex_lock(&hw_access, K_FOREVER);
			boton = tm1638_get_button();
			k_mutex_unlock(&hw_access);
			/* Solo reaccionamos en el flanco de "recien presionado",
			* no en cada iteracion mientras se mantiene presionado.
			*/
			if (boton > 0 && boton != last_button) {
				switch (boton)
				{
				case 1:
					if(k_sem_take(&display, K_NO_WAIT)!=0) k_sem_give(&display);
					k_msgq_purge(&lec_disp);
					k_msgq_put(&lec_disp, &boton, K_NO_WAIT);
					k_msleep(50);
					break;
				case 2:
					if(k_sem_take(&display, K_NO_WAIT)!=0) k_sem_give(&display);
					k_msgq_purge(&lec_disp);
					k_msgq_put(&lec_disp, &boton, K_NO_WAIT);
					k_msleep(50);
					break;
				case 3:
					if(k_sem_take(&display, K_NO_WAIT)!=0) k_sem_give(&display);
					k_msgq_purge(&lec_disp);
					k_msgq_put(&lec_disp, &boton, K_NO_WAIT);
					k_msleep(50);
					break;
				default:
					break;
				}
			}
			last_button = boton;
		
			k_msleep(22);
		}
	}
}

void actualizar_hora(void){

	struct rtc_time time;
	
	if(!device_is_ready(rtc)){
		LOG_WRN("rtc no esta listo");
	}

	time.tm_year = 2026;
	time.tm_mon = 8;
	time.tm_mday = 31;
	time.tm_hour = 18;
	time.tm_min = 0;
	time.tm_sec = 0;

	int ret = rtc_set_time(rtc, &time);

	if (ret != 0){
		LOG_WRN("error al iniciar rtc");
	}

	while(1){
		ret = rtc_get_time(rtc, &time);
		if (ret != 0){
			LOG_WRN("error al leer hora");
		}
		k_msgq_purge(&hora_act);
		k_msgq_put(&hora_act, &time, K_NO_WAIT);
		LOG_DBG("%d/%d/%d %d:%d:%d", time.tm_mday, time.tm_mon, time.tm_year, time.tm_hour, time.tm_min, time.tm_sec);
		k_msleep(1000);
	}
}

void lectura_adc(void){
	uint16_t rawtemp, rawlum, rawtempant = 0, rawlumant = 0;

	float a = 0.1;

	int ret;

	struct lectura_raw lect;
	
	struct adc_sequence temperatura = {
		.buffer =&rawtemp,
		.buffer_size = sizeof(rawtemp)
	}, lumens ={
		.buffer = &rawlum,
		.buffer_size = sizeof(rawlum)
	};
	while (1){

		k_sem_take(&rawr, K_FOREVER);

		while(1){

			if (k_sem_take(&stoprawr, K_NO_WAIT)==0) break;
			ret = adc_sequence_init_dt(&tem, &temperatura);
			if (ret!=0){
				LOG_WRN("error al iniciar secuencia");
				break;
			}
			ret = adc_read(tem.dev, &temperatura);
			if (ret!=0){
				LOG_WRN("no se pudo leer adc0 %d", ret);
			}
			ret = adc_sequence_init_dt(&lum, &lumens);
			if (ret!=0){
				LOG_WRN("error al iniciar secuencia");
				break;
			}
			ret = adc_read(lum.dev, &lumens);
			if (ret!=0){
				LOG_WRN("no se pudo leer adc0 %d", ret);
			}

			lect.vollu = (a*rawlum)+((1-a)*rawlumant);
			lect.volte = (a*rawtemp)+ ((1-a)*rawtempant);
			rawtempant = lect.volte;
			rawlumant = lect.vollu;

			if (k_msgq_put(&lec_raw, &lect, K_NO_WAIT) != 0){
				LOG_DBG("cola llena enviando datos");
				k_sem_give(&conv);
				k_sem_give(&stoprawr);
			}

			k_msleep(50);
		}
	}
}

void prom_adc(void){
	uint16_t tempe [10], lume [10];
	uint16_t tempe_prom, lume_prom, tempe_ant = 0, lume_ant = 0;
	struct lectura_raw lera;
	struct lectura_fina fin;
	float a = 0.1;
	while (1){
		k_sem_take(&conv, K_FOREVER);
		while (1){
			 
			if (k_sem_take(&stopconv, K_NO_WAIT)==0) break;

			for (int i = 0; i < 10; i++){
				k_msgq_get(&lec_raw, &lera, K_NO_WAIT);
				tempe[i] = lera.volte;
				lume[i] = lera.vollu;
			}
			LOG_DBG("promediando lecturas");
			k_sem_give(&rawr);
			tempe_prom = 0;
			lume_prom = 0;
			for (int i = 0; i < 10; i++){
				tempe_prom += tempe[i];
				lume_prom += lume[i];
			}

			tempe_prom = (tempe_prom/10);
			lume_prom =lume_prom/10;
			tempe_ant = tempe_prom;
			lume_ant = lume_prom;

			fin.voltem = tempe_prom;
			fin.vollum = lume_prom;

			LOG_DBG("temp: %d lum: %d", fin.voltem, fin.vollum);

			k_msgq_peek(&hora_act, &fin.horaa);

			k_msgq_purge(&lec_fin);
			k_msgq_put(&lec_fin, &fin, K_NO_WAIT);

			k_sem_give(&stopconv);
		}
	}	
}

uint16_t convlum (int16_t raw){
	int res = 0, a = 22650;
	float exp;
	uint16_t lux = 0;
	res = (100*raw)/(4095-raw);
	exp = 0.470576029;
	lux = (uint16_t) (pow((a/res), (1/exp)));
	return lux;
}

uint16_t convtemp (int16_t raw){
	float a = 36.92, b = -1;
	uint16_t final =(uint16_t)((raw/a)+b);
	return final;
}

void display_task(void){
	struct lectura_fina lect;
	int var=0;
	int boton;
	while(1){
		k_sem_take(&display, K_FOREVER);
		while(1){

			if (k_sem_take(&stopdis, K_NO_WAIT)==0) break;

			k_msgq_peek(&lec_disp, &boton);
			
			k_msgq_peek(&lec_fin, &lect);

			switch (boton)
			{
			case 1:
				var = convtemp(lect.voltem);
				break;
			case 2:
				var = convlum(lect.vollum);
				break;
			case 3:
				var = lect.horaa.tm_hour*10000 + lect.horaa.tm_min*100 + lect.horaa.tm_sec;
				break;
			default:
				break;
			}

			k_mutex_lock(&hw_access, K_FOREVER);
			tm1638_clear();
			tm1638_display(var);
			k_mutex_unlock(&hw_access);

			LOG_INF("mostrando en display: %d\n", var);

			k_msleep(1000);

		}	
	}
}

K_THREAD_DEFINE (main_id, STACK_SIZE, mainloop, NULL, NULL, NULL,
	PRIORITY_MAIN, 0, 0);
K_THREAD_DEFINE (botones_id, STACK_SIZE, leer_botones, NULL, NULL, NULL,
	PRIORITY_READ_TM1638, 0, 0);
K_THREAD_DEFINE (hora_id, STACK_SIZE, actualizar_hora, NULL, NULL, NULL,
	PRIORITY_HORA, 0, 0);
K_THREAD_DEFINE (adc_id, STACK_SIZE, lectura_adc, NULL, NULL, NULL,
	PRIORITY_READ_ADC, 0, 0);
K_THREAD_DEFINE (prom_id, STACK_SIZE, prom_adc, NULL, NULL, NULL,
	PRIORITY_PROM_ADC, 0, 0);
K_THREAD_DEFINE (display_id, STACK_SIZE, display_task, NULL, NULL, NULL,
	PRIORITY_DISPLAY, 0, 0);