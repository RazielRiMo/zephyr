/*
 * Copyright (c) 2012-2014 Wind River Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/drivers/counter.h>
#include <stdlib.h>

#define USER_NODE DT_PATH(zephyr_user)

#define STACK_SIZE 512

#define ECCHO_PRIORITY 1
#define TRIGGER_PRIORITY 2

#define ECHO_TIMEOUT_US 30000U
#define VEL_SONIDO 343U

static const struct gpio_dt_spec trigger = GPIO_DT_SPEC_GET(USER_NODE, trigger_gpios);
static const struct gpio_dt_spec eccho = GPIO_DT_SPEC_GET(USER_NODE, eccho_gpios);
LOG_MODULE_REGISTER(puerta, LOG_LEVEL_INF);

K_SEM_DEFINE(launch,0,1);
K_SEM_DEFINE(start,0,1);
K_SEM_DEFINE(end,0,1);

static struct gpio_callback eccho_cb;
volatile uint32_t inicio, fin, us;
long dis;

void inicio_trigger (void){
	
	while(1){
		k_sem_take(&launch, K_FOREVER);
		gpio_pin_set_dt(&trigger, 1);
		k_busy_wait(10);
		gpio_pin_set_dt(&trigger, 0);
	}
}

void isr_eccho(const struct device *dev, struct gpio_callback *cb, uint32_t pins){
	if (gpio_pin_get_dt(&eccho) == 1) k_sem_give(&start);
	else k_sem_give(&end);
}

void inicio_timer(void){
	while(1){
		k_sem_take(&start,K_FOREVER);
		fin = k_cycle_get_32();
	}
}

void fin_timer(void){
	while(1){
		k_sem_take(&end, K_FOREVER);
		fin = k_cycle_get_32();
		us = k_cyc_to_us_floor32(inicio - fin);
		dis = (long) us / 58.2;
		if (dis >= 400 || dis<=2) LOG_ERR("FUERA DE RANGO");
		else LOG_INF("%ld cm", dis);
		k_sem_give(&launch);
	}
}

int main(void){
	if (!gpio_is_ready_dt(&trigger) || !gpio_is_ready_dt(&eccho)) {
		LOG_DBG("GPIO o timer no listo\n");
	}
	if (gpio_pin_configure_dt(&trigger, GPIO_OUTPUT_INACTIVE) < 0 ||
	    gpio_pin_configure_dt(&eccho, GPIO_INPUT) < 0) {
		LOG_DBG("Error configurando GPIO o timer\n");
		}
	gpio_pin_interrupt_configure_dt(&eccho, GPIO_INT_EDGE_BOTH);
	gpio_init_callback(&eccho_cb, isr_eccho, BIT(eccho.pin));
	gpio_add_callback_dt(&eccho, &eccho_cb);

	k_sem_give(&launch);
	return 0;
}

K_THREAD_DEFINE(iniciotrigger, 1024, inicio_trigger, NULL, NULL, NULL, 4, 0, 0);
K_THREAD_DEFINE(iniciotimer, 1024, inicio_timer, NULL, NULL, NULL, 3, 0, 0);
K_THREAD_DEFINE(fintimer, 1024, fin_timer, NULL, NULL, NULL, 2, 0, 0);