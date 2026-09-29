/*
 * telemetry_tx.c
 * ================
 * POR QUÉ este módulo existe: es la "Tarea de Envío (Simulación)" del
 * enunciado. Corre de forma completamente independiente del notificador
 * TCP (tcp_server.c) y de la tarea de procesamiento (rx_processor.c): las
 * tres son tareas Zephyr separadas que el scheduler intercala.
 *
 * CON QUÉ se comunica: con json_protocol.c (para construir la trama) y
 * con tcp_server.c (para enviarla por el socket del cliente conectado).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "telemetry_tx.h"
#include "tcp_server.h"
#include "rx_processor.h"
#include "json_protocol.h"
#include "network_config.h"

LOG_MODULE_REGISTER(telemetry_tx, LOG_LEVEL_INF);

/* PARÁMETROS de hilo: mismo criterio de tamaño/prioridad documentado en
 * tcp_server.c (4 KiB cubre los buffers locales de json_protocol.c;
 * prioridad 5 = misma prioridad que las otras dos tareas, el scheduler
 * las alterna por turnos). */
#define TX_STACK_SIZE 4096
#define TX_PRIORITY   5

static void telemetry_tx_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	/* 'seq' vive en la pila del hilo (no es global) porque solo esta
	 * tarea la usa; se declara aquí, fuera del bucle, para que persista
	 * entre iteraciones en vez de reiniciarse en 0 cada vez. */
	uint32_t seq = 0;

	for (;;) {
		char frame[JSON_FRAME_MAX_LEN];

		/* POR QUÉ "value" cambia cada ciclo: es el requisito
		 * explícito del enunciado ("cambiando los datos en cada
		 * ciclo para alterar dinámicamente el CRC"). Aquí se usa un
		 * patrón simple y determinista (0..99, con "%") en vez de un
		 * sensor real, para que el proyecto funcione sin hardware
		 * adicional; en un proyecto real, esta línea es el ÚNICO
		 * lugar que habría que tocar para leer un sensor de verdad
		 * (el resto del pipeline -- esquema, CRC, envío -- no
		 * cambia). Se mantiene como "int" (no float) para no
		 * depender de soporte de punto flotante en el enlazador. */
		int simulated_value = (int)(seq % 100);

		/* COMUNICACIÓN: construye la trama completa (payload + CRC
		 * + '\n') delegando TODO el trabajo de JSON/CRC a
		 * json_protocol.c. PARÁMETROS:
		 *   - seq: contador de esta tarea, para que la PC pueda
		 *     notar tramas perdidas/reordenadas.
		 *   - k_uptime_get(): milisegundos desde el arranque de la
		 *     ESP32 -- referencia temporal sin necesitar RTC externo;
		 *     se castea a uint32_t porque el esquema define
		 *     "uptime_ms" como entero de 32 bits (suficiente para
		 *     ~49 dias de uptime, más que de sobra para este proyecto).
		 *   - rx_processor_get_crc_error_count(): así la telemetría
		 *     también informa a la PC cuántos errores de CRC detectó
		 *     la ESP32 del lado de los comandos entrantes. */
		int frame_len = json_protocol_build_telemetry(
			frame, sizeof(frame), seq, (uint32_t)k_uptime_get(),
			simulated_value, rx_processor_get_crc_error_count());

		if (frame_len > 0) {
			/* COMUNICACIÓN: tcp_server_send() usa internamente el
			 * socket del cliente actualmente conectado (si lo
			 * hay); si no hay cliente, devuelve false y esta
			 * tarea simplemente sigue con el próximo ciclo -- no
			 * es un error, es el estado normal antes de que la
			 * PC se conecte. */
			if (!tcp_server_send(frame, (size_t)frame_len)) {
				LOG_DBG("Sin cliente conectado: telemetria no enviada");
			}
		} else {
			LOG_WRN("No se pudo construir la trama de telemetria (buffer insuficiente?)");
		}

		seq++;

		/* PARÁMETRO TELEMETRY_PERIOD_MS (network_config.h, por
		 * defecto 2000 ms): k_msleep() cede la CPU durante ese
		 * tiempo -- esta tarea NO consume ciclos de procesador
		 * mientras "duerme", dejando esos ciclos disponibles para
		 * tcp_server.c/rx_processor.c. */
		k_msleep(TELEMETRY_PERIOD_MS);
	}
}

/* Ver la explicación detallada de los parámetros de K_THREAD_DEFINE en
 * tcp_server.c. Igual que las otras dos tareas, se crea SUSPENDIDA
 * (K_FOREVER) para no intentar enviar nada antes de que la Wi-Fi tenga IP
 * y el servidor TCP ya exista. */
K_THREAD_DEFINE(telemetry_tx_tid, TX_STACK_SIZE, telemetry_tx_thread_fn,
		 NULL, NULL, NULL, TX_PRIORITY, 0, K_FOREVER);

void telemetry_tx_start(void)
{
	k_thread_start(telemetry_tx_tid);
}
