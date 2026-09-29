#ifndef TELEMETRY_TX_H_
#define TELEMETRY_TX_H_

/**
 * @brief Arranca la tarea periódica de envío de telemetría (creada
 *        suspendida con K_THREAD_DEFINE). Debe llamarse DESPUÉS de que la
 *        Wi-Fi tenga IP.
 *
 * Cada TELEMETRY_PERIOD_MS (network_config.h), esta tarea arma un objeto
 * de datos basado en el esquema modular (json_schema.h), lo codifica a
 * JSON con <data/json.h> (vía json_protocol_build_telemetry) y lo envía
 * al cliente TCP conectado, si lo hay. El valor simulado CAMBIA en cada
 * ciclo, por lo que el CRC de cada trama también cambia.
 */
void telemetry_tx_start(void);

#endif /* TELEMETRY_TX_H_ */
