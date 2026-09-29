#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "rx_processor.h"
#include "tcp_server.h"
#include "json_config.h"

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
		
		struct datos_rx result = {0};

		// 5. Comando: json_obj_parse
		// El porqué: Decodifica usando Zephyr de forma nativa hacia nuestro struct flexible.
		int ret = json_obj_parse(start, line_len, rx_descr, rx_descr_len, &result);

		if (ret < 0) {
			LOG_WRN("Trama descartada: Error de parseo JSON nativo (ret=%d)", ret);
		} else {
			// 6. Comando: calculate_crc_rx
			// El porqué: Valida integridad contra la firma de las variables útiles esperadas.
			int expected_crc = calculate_crc_rx(&result);
			if (expected_crc != result.crc) {
				crc_error_count++;
				LOG_WRN("ERROR DE CRC. Recibido: 0x%04X, Calculado: 0x%04X", result.crc, expected_crc);
			} else {
				LOG_INF("Trama valida (comando_led=%d, setpoint=%d)", result.comando_led, result.setpoint_temp);
				if (local_action_cb != NULL) {
					local_action_cb(result.comando_led, true);
				}
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
		// 7. Comando: k_sem_take
		// El porqué: Sincroniza Bottom-Half, logrando que el parseo JSON pesado no ocurra en la simulación de ISR de Red.
		k_sem_take(&tcp_server_rx_ready, K_FOREVER);

		size_t n;
		while ((n = tcp_server_drain_rx(chunk, sizeof(chunk))) > 0) {
			if (accum_len + n >= sizeof(accum)) {
				LOG_WRN("Buffer lleno, descartando data");
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
		 NULL, NULL, NULL, RX_PROCESSOR_PRIORITY, 0, -1);

void rx_processor_start(rx_local_action_cb_t on_command)
{
	local_action_cb = on_command;
	k_thread_start(rx_processor_tid);
}
