#include "json_protocol.h"

const struct json_obj_descr tx_telemetry_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct tx_telemetry, seq, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct tx_telemetry, uptime_ms, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct tx_telemetry, value, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct tx_telemetry, esp32_crc_errors, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct tx_telemetry, crc, JSON_TOK_NUMBER),
};
const size_t tx_telemetry_descr_len = ARRAY_SIZE(tx_telemetry_descr);

const struct json_obj_descr rx_command_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct rx_command, cmd_val, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct rx_command, crc, JSON_TOK_NUMBER),
};
const size_t rx_command_descr_len = ARRAY_SIZE(rx_command_descr);
