#ifndef JSON_PROTOCOL_H_
#define JSON_PROTOCOL_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Tamaño máximo de una trama JSON (incluye el campo "crc" y el '\0'). */
#define JSON_FRAME_MAX_LEN 256

/**
 * Resultado de validar una trama JSON entrante.
 */
struct json_frame_result {
	bool format_ok;      /* true si se encontró el marcador ,"crc":"XXXX" */
	bool crc_ok;          /* true si crc_computed == crc_received */
	uint16_t crc_received;
	uint16_t crc_computed;
	bool has_value;        /* true si el campo entero "value" estaba presente */
	int value;
};

/**
 * @brief Construye una trama de telemetría JSON con su CRC-16 embebido.
 *
 * Trama de salida (ejemplo):
 *   {"type":"telemetry","seq":12,"uptime_ms":34567,"value":42,
 *    "esp32_crc_errors":0,"crc":"3F2A"}\n
 *
 * El CRC se calcula sobre el objeto JSON tal como quedaría SIN el campo
 * "crc" (cerrado con la '}' que sigue a "esp32_crc_errors"). Ver el
 * comentario de diseño en json_protocol.c para el detalle exacto.
 *
 * @return Longitud de la trama generada (excluyendo el '\0'), o -1 si el
 *         buffer de salida era demasiado pequeño.
 */
int json_build_telemetry_frame(char *out, size_t out_size,
				uint32_t seq, uint32_t uptime_ms,
				int value, uint32_t esp32_crc_errors);

/**
 * @brief Valida el CRC de una trama JSON recibida y extrae el campo "value".
 *
 * Estrategia deliberada: en vez de re-serializar el JSON (lo que exigiría
 * un parser + un formateador canónico en un microcontrolador con recursos
 * limitados), se localiza el marcador ,"crc":"XXXX" en la trama CRUDA, se
 * recalcula el CRC sobre los bytes que lo preceden (más una '}' de cierre)
 * y se compara byte a byte con el CRC recibido. Esto es determinista y no
 * depende del orden de las claves ni de cómo formatee números el emisor.
 *
 * @param line Trama recibida, SIN el salto de línea final.
 * @param len  Longitud de 'line' en bytes.
 */
void json_validate_frame(const char *line, size_t len,
			  struct json_frame_result *result);

#endif /* JSON_PROTOCOL_H_ */
