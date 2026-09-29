#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "telemetry_tx.h"
#include "tcp_server.h"
#include "json_config.h"

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
		int temp_simulada = 20 + (seq % 15);
		int hum_simulada = 50 + (seq % 20);

		// 2. Comando: struct datos_tx tx_datos = {...}
		// El porqué: Llena la estructura de datos agnóstica a JSON definida en json_config.h
		// La comunicación: Manejo de datos interno para telemetría.
		// Los parámetros: Variables simuladas.
		struct datos_tx tx_datos = {
			.id_dispositivo = 101,
			.temperatura = temp_simulada,
			.humedad = hum_simulada,
			.crc = 0 // Se calcula abajo
		};

		// 3. Comando: calculate_crc_tx
		// El porqué: Sella matemáticamente los datos usando el algoritmo XOR acumulativo.
		tx_datos.crc = calculate_crc_tx(&tx_datos);

		// 4. Comando: json_calc_encoded_len & json_obj_encode_buf
		// El porqué: Convierte nativamente el struct C a un JSON válido usando <data/json.h>.
		ssize_t frame_len = json_calc_encoded_len(tx_descr, tx_descr_len, &tx_datos);

		if (frame_len > 0 && frame_len < sizeof(frame) - 2) {
			json_obj_encode_buf(tx_descr, tx_descr_len, &tx_datos, frame, sizeof(frame));
			
			frame[frame_len] = '\n';
			frame[frame_len + 1] = '\0';
			frame_len++;

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
