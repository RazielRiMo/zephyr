#ifndef JSON_PROTOCOL_H_
#define JSON_PROTOCOL_H_

#include <zephyr/data/json.h>
#include <stdint.h>
#include <stdbool.h>

#define JSON_FRAME_MAX_LEN 256

/* Estructura para TX (Telemetría) */
struct tx_telemetry {
	int seq;
	int uptime_ms;
	int value;
	int esp32_crc_errors;
	int crc;
};

extern const struct json_obj_descr tx_telemetry_descr[];
extern const size_t tx_telemetry_descr_len;

/* Estructura para RX (Comandos) */
struct rx_command {
	int cmd_val;
	int crc;
};

extern const struct json_obj_descr rx_command_descr[];
extern const size_t rx_command_descr_len;

static inline int calculate_simple_crc(int val) {
    return (val ^ 0xAAAA) & 0xFFFF;
}

#endif /* JSON_PROTOCOL_H_ */
