/*
 * main.c - Puente TCP/JSON entre una ESP32 (Zephyr RTOS) y una PC.
 *
 * Responsabilidades de este archivo:
 *   1. Conectar la Wi-Fi (wifi_manager.c) y esperar IP por DHCP.
 *   2. Arrancar el servidor TCP (tcp_server.c), que valida el CRC de cada
 *      trama entrante y, si es válida, invoca apply_local_action().
 *   3. En un bucle periódico, armar y enviar una trama JSON de telemetría
 *      con CRC (json_protocol.c) al cliente conectado (si lo hay).
 */

#include <stdint.h>
#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>

#include "wifi_manager.h"
#include "tcp_server.h"
#include "json_protocol.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* LED que materializa la "accion local" pedida por la PC. Definido en
 * app.overlay como alias "action-led"; ajusta el pin alli si tu placa lo
 * requiere. */
#define ACTION_LED_NODE DT_ALIAS(action_led)
static const struct gpio_dt_spec action_led = GPIO_DT_SPEC_GET(ACTION_LED_NODE, gpios);

/*
 * Se ejecuta en el contexto del hilo del servidor TCP cada vez que llega
 * una trama JSON con CRC valido y un campo "value" numerico.
 *
 * Aqui interpretamos "value" como un numero de parpadeos del LED local,
 * acotado por seguridad. Sustituye el cuerpo de esta funcion por la accion
 * real que necesites (activar un rele, mover un actuador, ajustar un ciclo
 * de trabajo PWM mediante el driver LEDC de la ESP32, etc.) sin tener que
 * tocar nada del protocolo de red/CRC.
 */
static void apply_local_action(int value, bool has_value)
{
	if (!has_value) {
		LOG_WRN("Trama valida pero sin campo \"value\": no hay accion que ejecutar");
		return;
	}

	int blinks = CLAMP(value, 0, 20);

	LOG_INF("Ejecutando accion local: %d parpadeo(s) del LED (value recibido=%d)",
		blinks, value);

	for (int i = 0; i < blinks; i++) {
		gpio_pin_set_dt(&action_led, 1);
		k_msleep(120);
		gpio_pin_set_dt(&action_led, 0);
		k_msleep(120);
	}
}

int main(void)
{
	LOG_INF("=== Puente TCP/JSON ESP32 <-> PC (Zephyr RTOS) ===");

	if (!gpio_is_ready_dt(&action_led)) {
		LOG_ERR("El GPIO del LED de accion no esta listo (revisa app.overlay)");
	} else {
		gpio_pin_configure_dt(&action_led, GPIO_OUTPUT_INACTIVE);
	}

	int ret = wifi_manager_connect();

	if (ret != 0) {
		LOG_ERR("No fue posible conectar a la red Wi-Fi (err=%d). Deteniendo.", ret);
		return ret;
	}

	tcp_server_start(apply_local_action);

	uint32_t seq = 0;

	for (;;) {
		char frame[JSON_FRAME_MAX_LEN];

		/* Valor de telemetria simulado (patron 0..99). Sustituyelo por
		 * una lectura real (p. ej. un sensor por I2C) si tu proyecto
		 * lo incluye; mantén el valor como entero para no depender de
		 * soporte de punto flotante en printf/snprintf del firmware. */
		int simulated_value = (int)(seq % 100);

		int frame_len = json_build_telemetry_frame(
			frame, sizeof(frame), seq, (uint32_t)k_uptime_get(),
			simulated_value, tcp_server_get_crc_error_count());

		if (frame_len > 0) {
			if (!tcp_server_send(frame, (size_t)frame_len)) {
				LOG_DBG("Sin cliente conectado: telemetria no enviada");
			}
		}

		seq++;
		k_msleep(CONFIG_APP_TELEMETRY_PERIOD_MS);
	}

	return 0;
}
