/*
 * rx_processor.c
 * ================
 * POR QUÉ este módulo existe: es la "tarea de procesamiento específica"
 * que pide el enunciado. Pasa casi todo su tiempo BLOQUEADA en
 * k_sem_take(&tcp_server_rx_ready, K_FOREVER) — el semáforo binario que
 * tcp_server.c libera al recibir datos (ver el comentario de diseño en
 * ese archivo, que explica por qué ese hilo hace de "ISR de aplicación").
 *
 * Al despertar, este módulo:
 *   1. Drena TODOS los bytes pendientes del buffer de tcp_server.c
 *      (tcp_server_drain_rx), en un bucle propio (por si llegó más de una
 *      ráfaga desde la última señal: un semáforo binario no cuenta
 *      eventos, solo indica "hay trabajo pendiente").
 *   2. Reensambla tramas completas delimitadas por '\n' en un buffer
 *      local (TCP es un flujo de bytes: una trama puede llegar partida en
 *      varios recv(), o varias tramas pueden llegar juntas).
 *   3. Para cada trama completa: decodifica y valida su CRC-16 llamando a
 *      json_protocol_decode_command() (que usa <data/json.h>, nunca
 *      parsing manual). Si falla, imprime un mensaje de error detallado y
 *      aumenta el contador local de errores. Si es válida, extrae
 *      "value" y llama al callback de acción local.
 *
 * CON QUÉ se comunica: tcp_server.c (semáforo + drenado del buffer),
 * json_protocol.c (decodificación/verificación) y, vía el callback
 * registrado en rx_processor_start(), con main.c (acción local sobre el
 * LED).
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "rx_processor.h"
#include "tcp_server.h"
#include "json_protocol.h"

LOG_MODULE_REGISTER(rx_processor, LOG_LEVEL_INF);

/* PARÁMETROS de hilo: mismo tamaño/prioridad que tcp_server.c (ver la
 * justificación detallada en ese archivo) -- aquí el mayor consumo de
 * pila no es la red sino json_obj_parse()/json_obj_encode_buf() operando
 * sobre buffers locales de hasta JSON_FRAME_MAX_LEN bytes. */
#define RX_PROCESSOR_STACK_SIZE 4096
#define RX_PROCESSOR_PRIORITY   5

/* PARÁMETRO ACCUM_BUF_SIZE: buffer de reensamblado de esta tarea. Se usa
 * JSON_FRAME_MAX_LEN (definido en json_protocol.h) para que el límite de
 * "cuánto puede crecer una trama antes de considerarse un error" esté
 * definido en UN solo lugar, coherente con el resto del protocolo. */
#define ACCUM_BUF_SIZE          JSON_FRAME_MAX_LEN

/* PARÁMETRO DRAIN_CHUNK_SIZE: tamaño del buffer intermedio usado para
 * sacar datos de tcp_server.c hacia 'accum'. No necesita ser grande: el
 * bucle de drenado (ver abajo) lo llama repetidamente hasta vaciar todo
 * lo pendiente. */
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
		/* COMUNICACIÓN: memchr busca el delimitador de FRAMING
		 * ('\n') dentro de lo acumulado hasta ahora; es la forma de
		 * saber "dónde termina una trama JSON completa" sobre un
		 * flujo de bytes que no tiene ese concepto por sí mismo. */
		char *newline = memchr(start, '\n', (size_t)(end - start));

		if (newline == NULL) {
			break; /* la trama actual todavia no llego completa */
		}

		size_t line_len = (size_t)(newline - start);
		struct json_command_result result;

		/* COMUNICACIÓN: única llamada a la capa de protocolo/JSON de
		 * este archivo. 'start' se pasa TAL CUAL (no const): ver la
		 * explicación de por qué en json_protocol.h/.c (decodifica
		 * in-place). Tras esta llamada, el contenido de
		 * start[0..line_len) puede estar modificado (NULs insertados
		 * donde terminaban strings JSON) — por eso no se reutiliza
		 * ese rango para nada más que result. */
		json_protocol_decode_command(start, line_len, &result);

		if (!result.format_ok) {
			LOG_WRN("Trama descartada: no se pudo decodificar como "
				"{cmd, value, crc} valido");
		} else if (!result.crc_ok) {
			crc_error_count++;
			LOG_WRN("=== ERROR DE CRC EN TRAMA RECIBIDA DESDE LA PC ===");
			LOG_WRN("  CRC recibido  : 0x%04X", result.crc_received);
			LOG_WRN("  CRC calculado : 0x%04X", result.crc_computed);
			LOG_WRN("  Trama (%d bytes) descartada por integridad", (int)line_len);
			LOG_WRN("  Contador total de errores de CRC: %u", crc_error_count);
		} else {
			LOG_INF("Trama valida recibida (value=%d)", result.value);
			if (local_action_cb != NULL) {
				/* COMUNICACIÓN: invoca el callback registrado
				 * por main.c (apply_local_action) desde ESTE
				 * hilo (rx_processor), nunca desde el hilo
				 * notificador de tcp_server.c. */
				local_action_cb(result.value, result.has_value);
			}
		}

		start = newline + 1;
	}

	/* Conserva el resto incompleto (si lo hay) al inicio del buffer,
	 * para completarlo con la próxima ronda de drenado. */
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
		/* PARÁMETRO K_FOREVER: bloquea indefinidamente (0% de CPU
		 * mientras no hay datos) hasta que tcp_server.c (el
		 * "notificador", equivalente de aplicación a una ISR)
		 * libere el semaforo. Este es EXACTAMENTE el patrón
		 * "interrupción -> semáforo -> tarea bloqueada" pedido en
		 * el enunciado. */
		k_sem_take(&tcp_server_rx_ready, K_FOREVER);

		size_t n;

		/* Bucle de drenado: una sola señal del semáforo puede
		 * representar más de una ráfaga de datos ya acumulada en
		 * tcp_server.c, así que se sigue llamando a
		 * tcp_server_drain_rx() hasta que devuelva 0 (buffer vacío). */
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

/* Ver la explicación detallada de los parámetros de K_THREAD_DEFINE en
 * tcp_server.c; aquí aplica el mismo razonamiento: se crea SUSPENDIDA
 * (K_FOREVER) y rx_processor_start() la arranca explícitamente desde
 * main(), después de que la Wi-Fi ya tiene IP. */
K_THREAD_DEFINE(rx_processor_tid, RX_PROCESSOR_STACK_SIZE, rx_processor_thread_fn,
		 NULL, NULL, NULL, RX_PROCESSOR_PRIORITY, 0, K_FOREVER);

void rx_processor_start(rx_local_action_cb_t on_command)
{
	local_action_cb = on_command;
	k_thread_start(rx_processor_tid);
}
