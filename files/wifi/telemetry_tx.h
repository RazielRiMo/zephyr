#ifndef TELEMETRY_TX_H_
#define TELEMETRY_TX_H_

/**
 * @brief Arranca la tarea periódica de envío de telemetría (creada
 *        suspendida con K_THREAD_DEFINE). Debe llamarse DESPUÉS de que la
 *        Wi-Fi tenga IP.
 *
 * Cada CONFIG_APP_TELEMETRY_PERIOD_MS, esta tarea arma un JSON con un
 * valor simulado que CAMBIA en cada ciclo (por lo que el CRC de cada
 * trama también cambia) y lo envía al cliente TCP conectado, si lo hay.
 */
void telemetry_tx_start(void);

#endif /* TELEMETRY_TX_H_ */
