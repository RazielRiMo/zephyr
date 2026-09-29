#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <zephyr/data/json.h>
#include <zephyr/sys/util.h>

#include "json_protocol.h"
#include "json_schema.h"
#include "crc16.h"

/* Buffer intermedio para la codificación "solo payload" (sin "crc"), tanto
 * al construir telemetría como al re-codificar un comando para verificar
 * su CRC. 160 B alcanza de sobra para los campos de este esquema; si el
 * esquema crece, agranda esta cota junto con JSON_FRAME_MAX_LEN. */
#define PAYLOAD_BUF_LEN 160

int json_protocol_build_telemetry(char *out, size_t out_size,
				   uint32_t seq, uint32_t uptime_ms,
				   int value, uint32_t esp32_crc_errors)
{
	/* PASO 1 -- Codificar SOLO los datos (sin "crc") con <data/json.h>.
	 *
	 * POR QUÉ: para calcular un CRC que el receptor pueda reproducir,
	 * necesitamos los bytes EXACTOS de "los datos sin el crc". La única
	 * forma de tener esos bytes con certeza es pedírselos a la misma
	 * librería que los va a generar de verdad (json_obj_encode_buf),
	 * en vez de intentar adivinarlos con snprintf.
	 *
	 * COMUNICACIÓN: usa telemetry_payload_descr (json_schema.c), que
	 * describe los primeros 5 campos de struct telemetry_payload.
	 *
	 * PARÁMETROS de json_obj_encode_buf(descr, descr_len, val, buf, buf_size):
	 *   - descr/descr_len: el "mapa" de qué campos de 'val' escribir y
	 *     en qué orden (el orden del arreglo = el orden en el JSON).
	 *   - val: puntero al struct con los datos reales de ESTE ciclo.
	 *   - buf/buf_size: destino; la función devuelve error (no escribe
	 *     de más) si payload_buf fuera demasiado chico.
	 */
	struct telemetry_payload payload = {
		.type = "telemetry",
		.seq = (int)seq,
		.uptime_ms = (int)uptime_ms,
		.value = value,
		.esp32_crc_errors = (int)esp32_crc_errors,
	};
	char payload_buf[PAYLOAD_BUF_LEN];

	int ret = json_obj_encode_buf(telemetry_payload_descr, telemetry_payload_descr_len,
				       &payload, payload_buf, sizeof(payload_buf));
	if (ret != 0) {
		return -1;
	}

	/* PASO 2 -- CRC-16 sobre esos bytes exactos.
	 * PARÁMETRO len: strlen(payload_buf) es válido porque
	 * json_obj_encode_buf() deja el resultado como cadena C
	 * NUL-terminada (no un buffer binario de longitud separada). */
	uint16_t crc = crc16_ccitt_false((const uint8_t *)payload_buf, strlen(payload_buf));

	/* PASO 3 -- Formatear el CRC como texto hex de 4 dígitos.
	 *
	 * ACLARACIÓN IMPORTANTE: este snprintf() NO construye JSON (no hay
	 * '{', '}', ':' ni ',' en el formato "%04X") -- solo convierte un
	 * entero de 16 bits a su representación hexadecimal de ancho fijo,
	 * para guardarla en un campo de tipo string. Es una conversión de
	 * VALOR, no un armado de estructura JSON a mano; el objeto JSON en
	 * sí lo sigue generando exclusivamente json_obj_encode_buf() en el
	 * paso 4. */
	char crc_str[5];

	snprintf(crc_str, sizeof(crc_str), "%04X", crc);

	/* PASO 4 -- Codificar el frame COMPLETO (payload + crc) con
	 * <data/json.h>: esta es la trama que realmente sale por el socket.
	 * COMUNICACIÓN: usa telemetry_frame_descr, que agrega el campo
	 * "crc" (JSON_TOK_STRING) al final del mismo conjunto de campos. */
	struct telemetry_frame frame = {
		.type = payload.type,
		.seq = payload.seq,
		.uptime_ms = payload.uptime_ms,
		.value = payload.value,
		.esp32_crc_errors = payload.esp32_crc_errors,
		.crc = crc_str,
	};

	ret = json_obj_encode_buf(telemetry_frame_descr, telemetry_frame_descr_len,
				   &frame, out, out_size);
	if (ret != 0) {
		return -1;
	}

	size_t len = strlen(out);

	/* Framing por línea: TCP es un flujo de bytes SIN límites de mensaje
	 * propios (dos tramas seguidas podrían llegar "pegadas" al otro
	 * lado). Se añade '\n' como delimitador de TRANSPORTE -- no es
	 * contenido JSON, por eso se agrega DESPUÉS de que json_obj_encode_buf
	 * terminó, nunca dentro del objeto codificado. */
	if (len + 1 >= out_size) {
		return -1;
	}
	out[len] = '\n';
	out[len + 1] = '\0';

	return (int)(len + 1);
}

void json_protocol_decode_command(char *raw, size_t len, struct json_command_result *result)
{
	memset(result, 0, sizeof(*result));

	/* PASO 1 -- Decodificar el frame COMPLETO (cmd + value + crc) con
	 * <data/json.h>.
	 *
	 * POR QUÉ "raw" no es const: json_obj_parse() decodifica IN PLACE.
	 * Para cada campo JSON_TOK_STRING, sustituye la comilla de cierre
	 * por '\0' y deja el puntero correspondiente (frame.cmd, frame.crc)
	 * apuntando DENTRO de 'raw'. Esto evita copiar memoria (importante
	 * en un microcontrolador), pero significa que frame.cmd/frame.crc
	 * SOLO son válidos mientras 'raw' siga vivo y sin sobrescribir --
	 * por eso se usan y se descartan dentro de esta misma función,
	 * antes de que rx_processor.c reutilice ese buffer para la próxima
	 * trama.
	 *
	 * PARÁMETROS de json_obj_parse(json, len, descr, descr_len, val):
	 *   - json/len: EXACTAMENTE los bytes de un objeto JSON (sin '\n'
	 *     final); rx_processor.c ya se encarga de pasar la línea
	 *     recortada en el '\n'.
	 *   - descr/descr_len: command_frame_descr, que espera 3 campos:
	 *     cmd (string), value (número), crc (string).
	 *   - val: dónde escribir los punteros/enteros decodificados.
	 *
	 * VALOR DE RETORNO: no es un simple 0/-1. Si el texto no es JSON
	 * válido, devuelve un código negativo. Si es válido, devuelve una
	 * MÁSCARA DE BITS: el bit N indica que el campo descr[N] fue
	 * encontrado. Con 3 campos, "los 3 presentes" es
	 * BIT(0)|BIT(1)|BIT(2) -- por eso se compara contra esa máscara en
	 * vez de solo comprobar "ret >= 0" (un JSON válido pero incompleto,
	 * p. ej. sin "crc", NO debe tratarse como éxito). */
	struct command_frame frame = {0};

	int parsed_mask = json_obj_parse(raw, len, command_frame_descr,
					  command_frame_descr_len, &frame);
	const int expected_mask = BIT(0) | BIT(1) | BIT(2);

	if (parsed_mask < 0 || (parsed_mask & expected_mask) != expected_mask) {
		result->format_ok = false;
		return;
	}
	result->format_ok = true;

	/* PASO 2 -- Re-codificar SOLO cmd+value (sin "crc") para reproducir
	 * los bytes sobre los que el EMISOR debió calcular el CRC.
	 *
	 * POR QUÉ este paso, en vez de "recortar" el texto crudo a mano:
	 * mantiene TODO el manejo de JSON dentro de <data/json.h> (el
	 * enunciado prohíbe construir/parsear JSON a mano). Como
	 * command_payload_descr describe los MISMOS 2 primeros campos, en
	 * el MISMO orden que usó el emisor (ver protocol.py en la PC), el
	 * resultado de esta re-codificación es byte-a-byte idéntico al que
	 * produjo el emisor -- siempre que ambos lados usen separadores
	 * compactos (ver la nota sobre esto en protocol.py).
	 *
	 * COMUNICACIÓN: payload.cmd apunta al mismo texto que frame.cmd
	 * (dentro de 'raw'); es un simple alias de puntero, no una copia. */
	struct command_payload payload = {
		.cmd = frame.cmd,
		.value = frame.value,
	};
	char verify_buf[PAYLOAD_BUF_LEN];

	int ret = json_obj_encode_buf(command_payload_descr, command_payload_descr_len,
				       &payload, verify_buf, sizeof(verify_buf));
	if (ret != 0) {
		result->format_ok = false;
		return;
	}

	/* PASO 3 -- Comparar el CRC calculado localmente contra el recibido. */
	result->crc_computed = crc16_ccitt_false((const uint8_t *)verify_buf, strlen(verify_buf));
	/* PARÁMETRO base 16: frame.crc es texto hexadecimal ("7A3F"), tal
	 * como lo produce "%04X" del lado emisor (ver json_protocol_build_telemetry
	 * y protocol.py). strtoul con NULL como segundo argumento indica
	 * "no necesito saber dónde terminó de leer". */
	result->crc_received = (uint16_t)strtoul(frame.crc, NULL, 16);
	result->crc_ok = (result->crc_computed == result->crc_received);

	result->value = frame.value;
	result->has_value = true; /* 'value' es un campo obligatorio de este esquema */
}
