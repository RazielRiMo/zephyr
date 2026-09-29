#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "json_protocol.h"
#include "crc16.h"

/* Marcador que separa el "payload" protegido por CRC del campo del CRC
 * mismo. Debe coincidir EXACTAMENTE con el usado en protocol.py. */
#define CRC_MARKER      ",\"crc\":\""
#define CRC_MARKER_LEN  (sizeof(CRC_MARKER) - 1)
#define CRC_HEX_LEN     4 /* 4 dígitos hex en mayúsculas, p.ej. "3F2A" */

int json_build_telemetry_frame(char *out, size_t out_size,
				uint32_t seq, uint32_t uptime_ms,
				int value, uint32_t esp32_crc_errors)
{
	char payload[JSON_FRAME_MAX_LEN];

	/* 1. Se arma el objeto JSON COMPLETO (sin el campo "crc" todavía).
	 *    'value' cambia en cada llamada (ver telemetry_tx.c), por lo que
	 *    el CRC resultante también cambia trama a trama. */
	int payload_len = snprintf(payload, sizeof(payload),
		"{\"type\":\"telemetry\",\"seq\":%u,\"uptime_ms\":%u,"
		"\"value\":%d,\"esp32_crc_errors\":%u}",
		seq, uptime_ms, value, esp32_crc_errors);

	if (payload_len <= 0 || (size_t)payload_len >= sizeof(payload)) {
		return -1;
	}

	/* 2. El CRC se calcula sobre 'payload' completo, incluida la '}'
	 *    final. */
	uint16_t crc = crc16_ccitt_false((const uint8_t *)payload, (size_t)payload_len);

	/* 3. La trama final se obtiene sustituyendo esa última '}' por
	 *    ,"crc":"XXXX"} — de ahí el "%.*s" con precisión payload_len-1,
	 *    que imprime 'payload' SIN su último carácter. */
	int total_len = snprintf(out, out_size, "%.*s,\"crc\":\"%04X\"}\n",
				  payload_len - 1, payload, crc);

	if (total_len <= 0 || (size_t)total_len >= out_size) {
		return -1;
	}

	return total_len;
}

/* Busca "key":<entero> dentro de un objeto JSON plano de un solo nivel y
 * extrae el valor entero. Deliberadamente NO es un parser JSON genérico:
 * este protocolo solo necesita leer un campo entero conocido, y mantener
 * esto simple evita depender de una librería JSON completa en el firmware. */
static bool extract_int_field(const char *json, size_t len, const char *key, int *out_value)
{
	char pattern[32];
	int pattern_len = snprintf(pattern, sizeof(pattern), "\"%s\":", key);

	if (pattern_len <= 0 || (size_t)pattern_len >= sizeof(pattern)) {
		return false;
	}

	for (size_t i = 0; i + (size_t)pattern_len <= len; i++) {
		if (memcmp(&json[i], pattern, (size_t)pattern_len) != 0) {
			continue;
		}

		size_t pos = i + (size_t)pattern_len;

		while (pos < len && isspace((unsigned char)json[pos])) {
			pos++;
		}

		char numbuf[16];
		size_t n = 0;

		if (pos < len && (json[pos] == '-' || json[pos] == '+')) {
			numbuf[n++] = json[pos++];
		}
		while (pos < len && isdigit((unsigned char)json[pos]) && n < sizeof(numbuf) - 1) {
			numbuf[n++] = json[pos++];
		}
		numbuf[n] = '\0';

		if (n == 0) {
			return false;
		}

		*out_value = atoi(numbuf);
		return true;
	}

	return false;
}

void json_validate_frame(const char *line, size_t len, struct json_frame_result *result)
{
	memset(result, 0, sizeof(*result));

	/* 1. Ubicar el marcador ,"crc":" en la trama cruda. */
	const char *marker = NULL;

	for (size_t i = 0; i + CRC_MARKER_LEN <= len; i++) {
		if (memcmp(&line[i], CRC_MARKER, CRC_MARKER_LEN) == 0) {
			marker = &line[i];
			break;
		}
	}

	if (marker == NULL) {
		result->format_ok = false;
		return;
	}

	size_t protected_len = (size_t)(marker - line);
	size_t crc_hex_offset = protected_len + CRC_MARKER_LEN;

	if (crc_hex_offset + CRC_HEX_LEN > len || protected_len + 1 >= JSON_FRAME_MAX_LEN) {
		result->format_ok = false;
		return;
	}

	/* 2. Extraer los 4 dígitos hex del CRC recibido. */
	char crc_hex[CRC_HEX_LEN + 1];

	memcpy(crc_hex, &line[crc_hex_offset], CRC_HEX_LEN);
	crc_hex[CRC_HEX_LEN] = '\0';
	result->crc_received = (uint16_t)strtoul(crc_hex, NULL, 16);

	/* 3. Reconstruir el bloque protegido: todo lo anterior al marcador,
	 *    más una '}' de cierre (simétrico a json_build_telemetry_frame). */
	char protected_block[JSON_FRAME_MAX_LEN];

	memcpy(protected_block, line, protected_len);
	protected_block[protected_len] = '}';

	result->crc_computed = crc16_ccitt_false((const uint8_t *)protected_block, protected_len + 1);
	result->format_ok = true;
	result->crc_ok = (result->crc_computed == result->crc_received);

	/* 4. Extraer "value" del bloque ya reconstruido (independientemente
	 *    de si el CRC coincidió; es responsabilidad de quien llama
	 *    decidir si confía en 'value' cuando crc_ok es false). */
	result->has_value = extract_int_field(protected_block, protected_len + 1, "value", &result->value);
}
