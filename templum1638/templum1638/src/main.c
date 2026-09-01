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

#define PRIORITY_MAIN 5
#define PRIORITY_READ_TEMP 4
#define PRIORITY_READ_LUM 4
#define PRIORITY_DISPLAY 3
#define PRIORITY_CONVERCION 2
#define PRIORITY_READ_TM1638 1

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
	int16_t voltem;
	int16_t vollum;
	struct rtc_time horaa;
};

struct lectura_raw {
	int16_t volte;
	int16_t vollu;
};

K_MSGQ_DEFINE(hora_act, sizeof(struct rtc_time),1 , 1);
K_MSGQ_DEFINE(lec_raw , sizeof(struct lectura_raw),10, 1);
K_MSGQ_DEFINE(lec_fin , sizeof(struct lectura_fina), 1, 1);

static const struct device *rtc = DEVICE_DT_GET(RTC_NODE);


void mainloop(void){
	int rec;
	if (!device_is_ready(tem.dev)){
		LOG_WRN("ADC no esta listo");
	}
	rec = adc_channel_setup_dt(&tem);
	if (rec!=0){
		LOC_WRN("ADC no se pudo configurar");
	}
	if (!device_is_ready(lum.dev)){
		LOG_WRN("ADC no esta listo");
	}
	rec = adc_channel_setup_dt(&tem);
	if (rec!=0){
		LOG_WRN("ADC no se pudo configurar");
	}
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
	int last_button, boton;

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
					if(k_sem_take(&stoplums, K_NO_WAIT)!=0) k_sem_give(&stoplums);
					if(k_sem_take(&horastop, K_NO_WAIT)!=0) k_sem_give(&horastop);
					k_sem_give(&rlums);
					k_msleep(50);
					break;
				case 2:
					if(k_sem_take(&stoptemp, K_NO_WAIT)!=0) k_sem_give(&stoptemp);
					if(k_sem_take(&horastop, K_NO_WAIT)!=0) k_sem_give(&horastop);
					k_sem_give(&rtemp);
					k_msleep(50);
					break;
				case 3:
					if(k_sem_take(&stoptemp, K_NO_WAIT)!=0) k_sem_give(&stoptemp);
					if(k_sem_take(&stoplums, K_NO_WAIT)!=0) k_sem_give(&stoplums);
					k_sem_give(&hora);
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
		k_msleep(1000);
	}
}

void lectura_adc(void){
	int16_t rawtemp, rawlum, rawtempant = 0, rawlumant = 0;
	int32_t mvtemp, mvlum;

	float a = 0.1;

	int ret, num;

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

			ret = adc_read(tem.dev, &temperatura);
			if (ret!=0){
				LOG_WRN("no se pudo leer adc0 %d", ret);
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
	int16_t tempe [10], lume [10];
	int16_t tempe_prom, lume_prom, tempe_ant = 0, lume_ant = 0;
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

			for (int i = 0; i < 10; i++){
				tempe_prom += tempe[i];
				lume_prom += lume[i];
			}

			tempe_prom = (a*(tempe_prom/10))+((1-a)*tempe_ant);
			lume_prom = (a*(lume_prom/10))+((1-a)*lume_ant);
			tempe_ant = tempe_prom;
			lume_ant = lume_prom;

			fin.voltem = tempe_prom;
			fin.vollum = lume_prom;

			k_msgq_peek(&hora_act, &fin.horaa);

			k_msgq_purge(&lec_fin);
			k_msgq_put(&lec_fin, &fin, K_NO_WAIT);

			k_sem_give(&stopconv);
		}
	}	
}

