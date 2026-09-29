/*
 * main.c - Puente TCP/JSON entre una ESP32 (Zephyr RTOS) y una PC.
 * =================================================================
 *
 * Arquitectura de tareas (una vez conectada la Wi-Fi):
 *
 *   [tcp_server_thread]  --recv()-->  notify_data_available()
 *         |  (rol de "ISR": trabajo minimo + k_sem_give)         |
 *         v                                                       v
 *   tcp_server_rx_ready (semaforo binario) <---- k_sem_take ---- [rx_processor_thread_fn]
 *                                                                  |
 *                                                json.h: decodifica + valida CRC
 *                                                                  |
 *                                                                  v
 *                                                       apply_local_action()  (aqui, en main.c)
 *
 *   [telemetry_tx_thread_fn] --> arma objeto (json_schema.h) --> json.h
 *                                codifica (json_protocol.c) --> tcp_server_send()
 *
 * main() SOLO se encarga de: (1) inicializar el GPIO del LED, (2) conectar
 * la Wi-Fi, y (3) arrancar las tres tareas; luego retorna. Las tareas,
 * creadas con K_THREAD_DEFINE en sus respectivos módulos, siguen
 * ejecutándose de forma independiente (Zephyr no las "mata" porque
 * main() haya terminado).
 *
 * CONFIGURACIÓN: SSID, contraseña, puerto TCP y período de telemetría
 * viven en network_config.h (#define simples) -- no en Kconfig/prj.conf.
 */

#include <stdint.h>
#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>

#include "network_config.h"
#include "wifi_manager.h"
#include "tcp_server.h"
#include "rx_processor.h"
#include "telemetry_tx.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* COMUNICACIÓN: DT_ALIAS(action_led) resuelve, en tiempo de COMPILACIÓN,
 * el nodo de devicetree con alias "action-led" definido en app.overlay.
 * GPIO_DT_SPEC_GET extrae de ahí el controlador GPIO + número de pin +
 * flags (GPIO_ACTIVE_HIGH) ya empaquetados en un struct gpio_dt_spec, sin
 * que este archivo necesite conocer el pin físico exacto (eso es
 * responsabilidad exclusiva de app.overlay -- así el LED es portable
 * entre placas distintas con solo editar el overlay). */
#define ACTION_LED_NODE DT_ALIAS(action_led)
static const struct gpio_dt_spec action_led = GPIO_DT_SPEC_GET(ACTION_LED_NODE, gpios);

/*
 * apply_local_action() -- la "acción local" que pide el enunciado al
 * recibir un valor numérico válido desde la PC.
 *
 * POR QUÉ vive en main.c y no en rx_processor.c: rx_processor.c conoce el
 * PROTOCOLO (CRC, JSON) pero no debería conocer QUÉ HACE la aplicación
 * con un valor válido -- esa es una decisión de la aplicación final
 * (main.c), no del pipeline de comunicación. Por eso se pasa como
 * callback (function pointer) a rx_processor_start(), en vez de que
 * rx_processor.c la llame por nombre directamente.
 *
 * COMUNICACIÓN: se ejecuta en el CONTEXTO DE LA TAREA DE PROCESAMIENTO
 * (rx_processor.c), nunca en el hilo notificador de tcp_server.c -- por
 * eso puede tardar unos milisegundos (parpadear un LED) sin afectar la
 * atención al socket.
 */
static void apply_local_action(int value, bool has_value)
{
	if (!has_value) {
		LOG_WRN("Trama valida pero sin campo \"value\": no hay accion que ejecutar");
		return;
	}

	/* PARÁMETRO CLAMP(value, 0, 20): acota el numero de parpadeos por
	 * seguridad/tiempo de respuesta -- un valor grande (o negativo,
	 * enviado por error desde la GUI) no debe trabar esta tarea durante
	 * mucho tiempo ni ejecutar un bucle con conteo negativo. */
	int blinks = CLAMP(value, 0, 20);

	LOG_INF("Ejecutando accion local: %d parpadeo(s) del LED (value recibido=%d)",
		blinks, value);

	for (int i = 0; i < blinks; i++) {
		/* PARÁMETRO 120 ms por semiperiodo: suficientemente lento
		 * para que el parpadeo sea visible a simple vista, sin
		 * alargar demasiado la ejecucion total del callback. */
		gpio_pin_set_dt(&action_led, 1);
		k_msleep(120);
		gpio_pin_set_dt(&action_led, 0);
		k_msleep(120);
	}
}

int main(void)
{
	LOG_INF("=== Puente TCP/JSON ESP32 <-> PC (Zephyr RTOS) ===");
	/* Se loguea el SSID configurado para que, si la conexion Wi-Fi
	 * falla, sea evidente desde el primer mensaje QUE credenciales
	 * (network_config.h) esta usando el firmware realmente compilado. */
	LOG_INF("SSID configurado: \"%s\" (edita WIFI_SSID en network_config.h para cambiarlo)",
		WIFI_SSID);

	/* PARÁMETRO GPIO_OUTPUT_INACTIVE: configura el pin como SALIDA y en
	 * estado inicial "inactivo" (apagado, considerando GPIO_ACTIVE_HIGH
	 * definido en app.overlay) -- evita que el LED quede encendido por
	 * defecto antes de la primera accion local. */
	if (!gpio_is_ready_dt(&action_led)) {
		LOG_ERR("El GPIO del LED de accion no esta listo (revisa app.overlay)");
	} else {
		gpio_pin_configure_dt(&action_led, GPIO_OUTPUT_INACTIVE);
	}

	/* COMUNICACIÓN: bloquea aquí hasta que wifi_manager.c confirme IP
	 * (o falle). Es una decisión de diseño deliberada: no tiene sentido
	 * arrancar las tareas de red (tcp_server, rx_processor,
	 * telemetry_tx) si todavía no hay una interfaz de red utilizable. */
	int ret = wifi_manager_connect();

	if (ret != 0) {
		LOG_ERR("No fue posible conectar a la red Wi-Fi (err=%d). Deteniendo.", ret);
		return ret;
	}

	/* Arranque de las 3 tareas. El orden entre rx_processor_start() y
	 * tcp_server_start() no es estrictamente crítico gracias a
	 * K_SEM_DEFINE/K_MUTEX_DEFINE (el semáforo y los mutex de
	 * tcp_server.c ya existen, inicializados en 0, desde ANTES de que
	 * main() empiece a correr) -- pero arrancar primero a quien va a
	 * ESCUCHAR el semáforo dejar el flujo de arranque más fácil de
	 * leer. telemetry_tx_start() se deja al final porque es la única de
	 * las tres que no tiene nada que "esperar": simplemente empieza su
	 * bucle periódico. */
	rx_processor_start(apply_local_action);
	tcp_server_start();
	telemetry_tx_start();

	LOG_INF("Sistema listo: 3 tareas activas "
		"(notificador TCP tipo ISR, procesador RX por semaforo, emisor de telemetria)");

	return 0;
}
