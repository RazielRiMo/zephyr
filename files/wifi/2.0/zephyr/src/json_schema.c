#include "json_schema.h"

/*
 * Cada línea de un descriptor es UN campo del JSON. JSON_OBJ_DESCR_PRIM
 * recibe (struct, nombre_del_campo_en_el_struct, tipo_de_token):
 *   - El nombre del campo del struct se usa TAMBIÉN como nombre de la
 *     clave JSON (por eso los structs en json_schema.h usan como nombre
 *     de miembro exactamente la clave que se quiere en el JSON, p. ej.
 *     "uptime_ms" produce la clave "uptime_ms").
 *   - JSON_TOK_STRING  -> el campo debe ser "const char *".
 *   - JSON_TOK_NUMBER  -> el campo debe ser un entero (aquí, "int").
 * La MACRO usa internamente offsetof()/sizeof() sobre el struct indicado,
 * así que un error de tipo (p. ej. usar JSON_TOK_NUMBER sobre un "const
 * char *") se detecta en tiempo de COMPILACIÓN, no en tiempo de ejecución.
 */

/* ---- Telemetría (ESP32 -> PC) ---- */

const struct json_obj_descr telemetry_payload_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct telemetry_payload, type, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct telemetry_payload, seq, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct telemetry_payload, uptime_ms, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct telemetry_payload, value, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct telemetry_payload, esp32_crc_errors, JSON_TOK_NUMBER),
};
/* PARÁMETRO: se exporta la CANTIDAD de entradas (no solo el arreglo)
 * porque un "extern struct json_obj_descr arr[];" en otro archivo NO
 * conserva el tamaño del arreglo (queda como tipo incompleto): cualquier
 * módulo que quiera usar ARRAY_SIZE() sobre este descriptor desde fuera
 * de este archivo necesita este valor ya calculado aquí, donde el
 * arreglo SÍ tiene tamaño conocido en tiempo de compilación. */
const size_t telemetry_payload_descr_len = ARRAY_SIZE(telemetry_payload_descr);

const struct json_obj_descr telemetry_frame_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct telemetry_frame, type, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct telemetry_frame, seq, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct telemetry_frame, uptime_ms, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct telemetry_frame, value, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct telemetry_frame, esp32_crc_errors, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct telemetry_frame, crc, JSON_TOK_STRING),
};
const size_t telemetry_frame_descr_len = ARRAY_SIZE(telemetry_frame_descr);

/* ---- Comando (PC -> ESP32) ---- */

const struct json_obj_descr command_payload_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct command_payload, cmd, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct command_payload, value, JSON_TOK_NUMBER),
};
const size_t command_payload_descr_len = ARRAY_SIZE(command_payload_descr);

const struct json_obj_descr command_frame_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct command_frame, cmd, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct command_frame, value, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct command_frame, crc, JSON_TOK_STRING),
};
const size_t command_frame_descr_len = ARRAY_SIZE(command_frame_descr);
