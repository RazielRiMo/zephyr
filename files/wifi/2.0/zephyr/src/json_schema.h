#ifndef JSON_SCHEMA_H_
#define JSON_SCHEMA_H_

#include <zephyr/data/json.h>

/*
 * json_schema.h / json_schema.c
 * ==============================
 * POR QUÉ este archivo existe (requisito de diseño "modular y
 * reutilizable" del enunciado): es el ÚNICO lugar del proyecto donde se
 * describe "de qué campos se compone cada trama JSON". Toda la lógica de
 * red, hilos, semáforos e interrupciones (tcp_server.c, rx_processor.c,
 * telemetry_tx.c) NO conoce estos campos uno por uno: solo le pasa a
 * json_protocol.c un puntero a un struct + su descriptor. Para agregar,
 * quitar o renombrar un campo en un futuro proyecto, basta con:
 *   1. Editar el struct correspondiente aquí abajo.
 *   2. Editar su `struct json_obj_descr[]` correspondiente en
 *      json_schema.c (una línea JSON_OBJ_DESCR_PRIM por campo).
 * Ningún otro archivo necesita cambiar.
 *
 * CON QUÉ se comunica: <zephyr/data/json.h> es la librería INTEGRADA de
 * Zephyr para codificar/decodificar JSON a partir de descriptores
 * estáticos (JSON_OBJ_DESCR_PRIM) en vez de parsers genéricos -- ideal
 * para microcontroladores porque no reserva memoria dinámica y valida el
 * esquema en tiempo de compilación (los descriptores usan offsetof()).
 * Está prohibido usar cJSON u otra librería externa; este archivo es la
 * prueba de que todo el manejo de JSON pasa por json.h.
 *
 * Patrón "payload" vs "frame" (se repite para cada tipo de trama):
 *   - *_payload: SOLO los campos de datos, SIN "crc". Se usa para
 *     calcular el CRC sobre bytes 100% controlados por json.h.
 *   - *_frame:   los mismos campos MÁS "crc". Es lo que realmente viaja
 *     por el socket. Ver json_protocol.c para el porqué de esta división.
 */

/* ---------- Telemetría: la ESP32 la ENVÍA a la PC ---------- */

/* PARÁMETROS de cada campo (por qué existe, qué representa):
 *   type              -- string fijo "telemetry": permite a la PC (o a un
 *                         futuro tercer tipo de trama) distinguir el
 *                         propósito del mensaje solo con leer un campo.
 *   seq               -- contador incremental: útil para detectar tramas
 *                         perdidas o reordenadas en la GUI.
 *   uptime_ms         -- k_uptime_get() de la ESP32: referencia temporal
 *                         simple sin depender de RTC.
 *   value             -- dato "simulado" que cambia cada ciclo (ver
 *                         telemetry_tx.c): es justamente lo que hace que
 *                         el CRC sea distinto en cada trama.
 *   esp32_crc_errors  -- contador de errores de CRC del LADO ESP32 (tramas
 *                         de comando recibidas con CRC inválido), para que
 *                         la GUI de Python también pueda mostrarlo.
 */
struct telemetry_payload {
	const char *type;
	int seq;
	int uptime_ms;
	int value;
	int esp32_crc_errors;
};

/* Igual que telemetry_payload, más el campo "crc" (string de 4 hex).
 * PARÁMETRO crc: se decodifica/codifica como JSON_TOK_STRING (no como
 * número) para conservar ceros a la izquierda (p. ej. "003F"), que se
 * perderían si se guardara como entero. */
struct telemetry_frame {
	const char *type;
	int seq;
	int uptime_ms;
	int value;
	int esp32_crc_errors;
	const char *crc;
};

extern const struct json_obj_descr telemetry_payload_descr[];
extern const size_t telemetry_payload_descr_len;
extern const struct json_obj_descr telemetry_frame_descr[];
extern const size_t telemetry_frame_descr_len;

/* ---------- Comando: la PC lo ENVÍA a la ESP32 ---------- */

/* PARÁMETROS:
 *   cmd   -- string fijo "set_value": identifica la intención del mensaje
 *            (deja espacio para agregar más "cmd" en el futuro sin romper
 *            el esquema, p. ej. "set_pwm", "reset_counters", ...).
 *   value -- entero que el usuario escribió en el campo de texto de la
 *            GUI de Tkinter; es el dato que dispara apply_local_action().
 */
struct command_payload {
	const char *cmd;
	int value;
};

struct command_frame {
	const char *cmd;
	int value;
	const char *crc;
};

extern const struct json_obj_descr command_payload_descr[];
extern const size_t command_payload_descr_len;
extern const struct json_obj_descr command_frame_descr[];
extern const size_t command_frame_descr_len;

#endif /* JSON_SCHEMA_H_ */
