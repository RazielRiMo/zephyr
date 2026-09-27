#ifndef TCP_SERVER_H_
#define TCP_SERVER_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Callback invocado por el hilo del servidor TCP cada vez que llega
 *        una trama JSON con CRC válido y un campo "value" numérico.
 *
 * Se ejecuta en el contexto del hilo del servidor TCP (no en un ISR), así
 * que puede tardar unos milisegundos (p. ej. parpadear un LED) sin
 * problema, pero no debe bloquear indefinidamente.
 */
typedef void (*tcp_server_on_command_t)(int value, bool has_value);

/**
 * @brief Arranca el hilo del servidor TCP (creado con K_THREAD_DEFINE en
 *        estado suspendido) y registra el callback de aplicación.
 *
 * Debe llamarse DESPUÉS de que la Wi-Fi tenga IP (ver wifi_manager.c),
 * para evitar intentar bind()/listen() antes de tener una interfaz lista.
 */
void tcp_server_start(tcp_server_on_command_t on_command);

/**
 * @brief Envía una trama ya construida (incluyendo el '\n' final) al
 *        cliente actualmente conectado, si lo hay.
 *
 * @return true si había un cliente conectado y se intentó el envío;
 *         false si no hay ningún cliente conectado en este momento.
 */
bool tcp_server_send(const char *frame, size_t len);

/**
 * @return Número total de tramas recibidas desde la PC cuyo CRC no
 *         coincidió con el calculado localmente.
 */
uint32_t tcp_server_get_crc_error_count(void);

#endif /* TCP_SERVER_H_ */
