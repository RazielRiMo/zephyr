/*
 * main.c - Puente TCP/JSON entre una ESP32 (Zephyr RTOS) y una PC.
 *
 * Arquitectura de tareas (una vez conectada la Wi-Fi):
 *
 *   [tcp_server_thread]  --recv()-->  notify_data_available()
 *         |  (rol de "ISR": trabajo minimo + k_sem_give)         |
 *         v                                                       v
 *   tcp_server_rx_ready (semaforo binario) <---- k_sem_take ---- [rx_processor_thread_fn]
 *                                                                  |
 *                                                     valida CRC, extrae "value"
 *                                                                  |
 *                                                                  v
 *                                                       apply_local_action()  (aqui, en main.c)
 *
 *   [telemetry_tx_thread_fn] --> arma JSON con dato simulado que cambia
 *                                cada ciclo --> tcp_server_send()
 *
 * main() solo se encarga de la inicialización (GPIO, Wi-Fi) y de arrancar
 * las tres tareas; luego retorna. Las tareas, creadas con K_THREAD_DEFINE,
 * siguen ejecutándose de forma independiente.
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
#include "rx_processor.h"
#include "telemetry_tx.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* LED que materializa la "accion local" pedida por la PC. Definido en
 * app.overlay como alias "action-led"; ajusta el pin alli si tu placa lo
 * requiere. */
#define ACTION_LED_NODE DT_ALIAS(action_led)
static const struct gpio_dt_spec action_led = GPIO_DT_SPEC_GET(ACTION_LED_NODE, gpios);

/*
 * Se ejecuta en el CONTEXTO DE LA TAREA DE PROCESAMIENTO (rx_processor.c),
 * nunca en el hilo notificador de tcp_server.c, cada vez que llega una
 * trama con CRC valido y un campo "value" numerico.
 *
 * Aqui interpretamos "value" como un numero de parpadeos del LED local,
 * acotado por seguridad. Sustituye el cuerpo de esta funcion por la accion
 * real que necesites (activar un rele, mover un actuador, ajustar un ciclo
 * de trabajo PWM mediante el driver LEDC de la ESP32, etc.) sin tocar nada
 * del protocolo de red, CRC o la sincronizacion por semaforo.
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
	LOG_INF("SSID configurado: \"%s\" (edita CONFIG_APP_WIFI_SSID en prj.conf para cambiarlo)",
		CONFIG_APP_WIFI_SSID);

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

	/* Orden de arranque: primero quien va a ESCUCHAR el semaforo
	 * (rx_processor), despues quien lo va a LIBERAR (tcp_server), y por
	 * ultimo la telemetria. El orden entre las dos primeras no es
	 * estrictamente critico gracias a K_SEM_DEFINE (el semaforo ya
	 * existe y vale 0 desde antes de main()), pero mantenerlo asi deja
	 * el flujo de arranque mas facil de leer. */
	rx_processor_start(apply_local_action);
	tcp_server_start();
	telemetry_tx_start();

	LOG_INF("Sistema listo: 3 tareas activas "
		"(notificador TCP tipo ISR, procesador RX por semaforo, emisor de telemetria)");

	return 0;
}
