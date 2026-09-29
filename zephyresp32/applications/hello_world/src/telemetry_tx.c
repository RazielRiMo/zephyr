/*
 * telemetry_tx.c
 *
 * Tarea periódica ("Tarea de Envío" del enunciado): cada
 * CONFIG_APP_TELEMETRY_PERIOD_MS arma una trama JSON con un valor
 * simulado que cambia en cada ciclo -- por lo tanto el CRC-16 de cada
 * trama también es distinto de la anterior -- y la envía por TCP.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "telemetry_tx.h"
#include "tcp_server.h"
#include "rx_processor.h"
#include "json_protocol.h"

LOG_MODULE_REGISTER(telemetry_tx, LOG_LEVEL_INF);

#define TX_STACK_SIZE 4096
#define TX_PRIORITY   5

static void telemetry_tx_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	uint32_t seq = 0;

	for (;;) {
		char frame[JSON_FRAME_MAX_LEN];

		/* Dato simulado que cambia en cada ciclo (patron 0..99). En
		 * un proyecto real, sustituye esta linea por una lectura de
		 * sensor; el resto del pipeline (armado de JSON + CRC +
		 * envio) no necesita cambiar. Se mantiene como entero para
		 * no depender de soporte de punto flotante en snprintf. */
		int simulated_value = (int)(seq % 100);

		int frame_len = json_build_telemetry_frame(
			frame, sizeof(frame), seq, (uint32_t)k_uptime_get(),
			simulated_value, rx_processor_get_crc_error_count());

		if (frame_len > 0) {
			if (!tcp_server_send(frame, (size_t)frame_len)) {
				LOG_DBG("Sin cliente conectado: telemetria no enviada");
			}
		}

		seq++;
		k_msleep(CONFIG_APP_TELEMETRY_PERIOD_MS);
	}
}

K_THREAD_DEFINE(telemetry_tx_tid, TX_STACK_SIZE, telemetry_tx_thread_fn,
		 NULL, NULL, NULL, TX_PRIORITY, 0, -1);

void telemetry_tx_start(void)
{
	k_thread_start(telemetry_tx_tid);
}
