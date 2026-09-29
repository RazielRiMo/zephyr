#ifndef RX_PROCESSOR_H_
#define RX_PROCESSOR_H_

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Callback invocado por la tarea de procesamiento cada vez que
 *        llega una trama con CRC válido y un campo "value" numérico.
 *
 * Se ejecuta en el contexto de la tarea de procesamiento (no en el hilo
 * notificador de tcp_server.c), así que puede tardar unos milisegundos
 * (p. ej. parpadear un LED) sin afectar la recepción de red.
 */
typedef void (*rx_local_action_cb_t)(int value, bool has_value);

/**
 * @brief Arranca la tarea de procesamiento (creada suspendida con
 *        K_THREAD_DEFINE) y registra el callback de acción local.
 *
 * Esta tarea es la "tarea de procesamiento específica" del enunciado:
 * pasa casi todo su tiempo bloqueada en k_sem_take(&tcp_server_rx_ready,
 * K_FOREVER) (0% de CPU mientras espera) y, al despertar, drena el
 * buffer de tcp_server.c, reensambla tramas completas por línea y para
 * cada una valida su CRC-16.
 */
void rx_processor_start(rx_local_action_cb_t on_command);

/**
 * @return Número total de tramas recibidas desde la PC cuyo CRC no
 *         coincidió con el calculado localmente.
 */
uint32_t rx_processor_get_crc_error_count(void);

#endif /* RX_PROCESSOR_H_ */
