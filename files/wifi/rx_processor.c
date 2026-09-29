/*
 * rx_processor.c
 *
 * Tarea de procesamiento dedicada: pasa casi todo su tiempo BLOQUEADA en
 * k_sem_take(&tcp_server_rx_ready, K_FOREVER) -- el semáforo binario que
 * tcp_server.c libera al recibir datos (ver el comentario de diseño en ese
 * archivo). Al despertar:
 *
 *   1. Drena TODOS los bytes pendientes del buffer de tcp_server.c
 *      (tcp_server_drain_rx), en un bucle propio (por si llegó más de una
 *      ráfaga desde la última señal: un semáforo binario no cuenta
 *      eventos, solo indica "hay trabajo pendiente").
 *   2. Reensambla tramas completas delimitadas por '\n' en un buffer local
 *      (TCP es un flujo de bytes: una trama puede llegar partida en varios
 *      recv(), o varias tramas pueden llegar juntas).
 *   3. Para cada trama completa: valida su CRC-16. Si falla, imprime un
 *      mensaje de error detallado y aumenta el contador local de errores.
 *      Si es válida, extrae "value" y llama al callback de acción local.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "rx_processor.h"
#include "tcp_server.h"
#include "json_protocol.h"

LOG_MODULE_REGISTER(rx_processor, LOG_LEVEL_INF);

#define RX_PROCESSOR_STACK_SIZE 4096
#define RX_PROCESSOR_PRIORITY   5
#define ACCUM_BUF_SIZE          JSON_FRAME_MAX_LEN
#define DRAIN_CHUNK_SIZE        128

static uint32_t crc_error_count;
static rx_local_action_cb_t local_action_cb;

uint32_t rx_processor_get_crc_error_count(void)
{
	return crc_error_count;
}

/* Busca y procesa todas las tramas completas ('\n'-terminadas) presentes
 * en 'buf'; conserva cualquier resto incompleto al inicio del buffer. */
static void process_complete_lines(char *buf, size_t *len)
{
	char *start = buf;
	char *end = buf + *len;

	for (;;) {
		char *newline = memchr(start, '\n', (size_t)(end - start));

		if (newline == NULL) {
			break;
		}

		size_t line_len = (size_t)(newline - start);
		struct json_frame_result result;

		json_validate_frame(start, line_len, &result);

		if (!result.format_ok) {
			LOG_WRN("Trama descartada: no se encontro el campo \"crc\"");
		} else if (!result.crc_ok) {
			crc_error_count++;
			LOG_WRN("=== ERROR DE CRC EN TRAMA RECIBIDA DESDE LA PC ===");
			LOG_WRN("  CRC recibido  : 0x%04X", result.crc_received);
			LOG_WRN("  CRC calculado : 0x%04X", result.crc_computed);
			LOG_WRN("  Trama (%d bytes) descartada por integridad", (int)line_len);
			LOG_WRN("  Contador total de errores de CRC: %u", crc_error_count);
		} else {
			LOG_INF("Trama valida recibida (value=%d, has_value=%d)",
				result.value, result.has_value);
			if (local_action_cb != NULL) {
				local_action_cb(result.value, result.has_value);
			}
		}

		start = newline + 1;
	}

	size_t remaining = (size_t)(end - start);

	memmove(buf, start, remaining);
	*len = remaining;
}

static void rx_processor_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	static char accum[ACCUM_BUF_SIZE];
	size_t accum_len = 0;
	char chunk[DRAIN_CHUNK_SIZE];

	for (;;) {
		/* Bloqueada aqui: 0% de CPU mientras no hay datos. Se
		 * despierta cuando tcp_server.c (el "notificador", equivalente
		 * de aplicacion a una ISR) libera el semaforo. */
		k_sem_take(&tcp_server_rx_ready, K_FOREVER);

		size_t n;

		while ((n = tcp_server_drain_rx(chunk, sizeof(chunk))) > 0) {
			if (accum_len + n >= sizeof(accum)) {
				LOG_WRN("Buffer de reensamblado lleno: se descarta "
					"el contenido acumulado (trama demasiado larga?)");
				accum_len = 0;
				continue;
			}

			memcpy(accum + accum_len, chunk, n);
			accum_len += n;

			process_complete_lines(accum, &accum_len);
		}
	}
}

K_THREAD_DEFINE(rx_processor_tid, RX_PROCESSOR_STACK_SIZE, rx_processor_thread_fn,
		 NULL, NULL, NULL, RX_PROCESSOR_PRIORITY, 0, K_FOREVER);

void rx_processor_start(rx_local_action_cb_t on_command)
{
	local_action_cb = on_command;
	k_thread_start(rx_processor_tid);
}
