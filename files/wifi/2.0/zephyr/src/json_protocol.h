#ifndef JSON_PROTOCOL_H_
#define JSON_PROTOCOL_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * json_protocol.h / json_protocol.c
 * ===================================
 * POR QUÉ: separa la LÓGICA de "cómo se firma y verifica una trama con
 * CRC" (que es siempre el mismo patrón, sin importar los campos) del
 * ESQUEMA de cada trama (que vive en json_schema.h/c). tcp_server.c,
 * rx_processor.c y telemetry_tx.c llaman a las dos funciones de abajo y
 * no necesitan saber NADA sobre JSON_OBJ_DESCR_PRIM, json_obj_parse, ni
 * los nombres de los campos: eso es exactamente lo que pide el enunciado
 * al hablar de "sin alterar la lógica central de red, hilos o
 * interrupciones" cuando se modifique el esquema.
 *
 * Patrón usado en ambas funciones (encoding y decoding), explicado una
 * sola vez aquí para no repetirlo en cada comentario de json_protocol.c:
 *   1. Se codifica/decodifica el "payload" (los campos SIN "crc") usando
 *      <data/json.h>.
 *   2. Se calcula CRC-16 sobre los bytes EXACTOS que produjo/produciría
 *      esa codificación (nunca sobre una reconstrucción manual).
 *   3. Se compara (al decodificar) o se agrega (al codificar) el campo
 *      "crc", también a través de <data/json.h>.
 */

/* Cota superior razonable para una trama de este protocolo (campos cortos,
 * sin arreglos ni objetos anidados): deja margen de sobra y evita reservar
 * buffers grandes en la pila de tareas con stacks acotados. */
#define JSON_FRAME_MAX_LEN 256

/** Resultado de decodificar y verificar una trama de comando entrante. */
struct json_command_result {
	bool format_ok;       /* true si el JSON tenía los 3 campos esperados (cmd, value, crc) */
	bool crc_ok;           /* true si crc_computed == crc_received */
	uint16_t crc_received;
	uint16_t crc_computed;
	bool has_value;         /* true si 'value' se pudo extraer (implica format_ok) */
	int value;
};

/**
 * @brief Construye una trama de telemetría (payload + CRC) codificada
 *        íntegramente con <data/json.h>, lista para enviar por el socket
 *        (incluye el '\n' final de framing).
 *
 * COMUNICACIÓN: la usa telemetry_tx.c, una vez por ciclo de su tarea
 * periódica. El resultado se pasa a tcp_server_send().
 *
 * PARÁMETROS:
 * @param out    Buffer de salida donde se escribe la trama completa.
 * @param out_size Tamaño de 'out'; debe ser >= JSON_FRAME_MAX_LEN para
 *               tener margen (ver el mismo #define en este header).
 * @param seq    Número de secuencia (telemetry_tx.c lleva la cuenta).
 * @param uptime_ms Milisegundos desde el arranque (k_uptime_get() en el
 *               llamador); se pasa como parámetro en vez de llamarlo aquí
 *               para mantener esta función independiente del reloj del
 *               sistema (más fácil de probar/reutilizar).
 * @param value  Dato simulado (o, en un proyecto real, una lectura de
 *               sensor) que cambia en cada llamada.
 * @param esp32_crc_errors Contador actual de errores de CRC del lado
 *               ESP32 (rx_processor_get_crc_error_count()), para que la
 *               PC también lo vea.
 * @return Longitud de la trama escrita en 'out' (bytes, incluye '\n'), o
 *         un valor negativo si algo falló (p. ej. buffer insuficiente).
 */
int json_protocol_build_telemetry(char *out, size_t out_size,
				   uint32_t seq, uint32_t uptime_ms,
				   int value, uint32_t esp32_crc_errors);

/**
 * @brief Decodifica una trama de comando y verifica su CRC, ambos pasos
 *        usando exclusivamente <data/json.h>.
 *
 * COMUNICACIÓN: la usa rx_processor.c, una vez por cada línea completa
 * ('\n'-terminada) que llega al buffer que tcp_server.c fue llenando.
 *
 * PARÁMETROS:
 * @param raw NO es "const": <data/json.h> decodifica IN PLACE (reemplaza
 *            comillas de cierre de cada string por '\0' para poder
 *            devolver punteros "const char *" que apuntan DENTRO de este
 *            mismo buffer, sin copiar memoria). Por eso el buffer debe
 *            seguir vivo mientras 'raw' se está procesando, y por eso
 *            esta función NO debe recibir un literal de cadena.
 * @param len  Cantidad de bytes de 'raw' a interpretar como UN objeto
 *             JSON (sin incluir el '\n' delimitador de trama).
 * @param result Resultado: qué se decodificó y si el CRC coincidió.
 */
void json_protocol_decode_command(char *raw, size_t len,
				   struct json_command_result *result);

#endif /* JSON_PROTOCOL_H_ */
