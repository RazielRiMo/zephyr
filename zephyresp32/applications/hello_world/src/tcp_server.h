#ifndef TCP_SERVER_H_
#define TCP_SERVER_H_

#include <stddef.h>
#include <stdbool.h>
#include <zephyr/kernel.h>

/**
 * Semáforo binario que cumple el rol de "interrupción" pedido en el
 * enunciado: se libera (k_sem_give) cada vez que el hilo notificador de
 * este módulo detecta y retira datos nuevos del socket TCP. La tarea de
 * procesamiento (rx_processor.c) se bloquea en k_sem_take() a la espera
 * de esta señal — nunca hace polling activo.
 *
 * Se declara con K_SEM_DEFINE a nivel de archivo en tcp_server.c, por lo
 * que ya está inicializado antes de que cualquier tarea arranque (no hay
 * condición de carrera posible con su uso vía extern aquí).
 */
extern struct k_sem tcp_server_rx_ready;

/**
 * @brief Arranca el hilo servidor TCP (creado suspendido con
 *        K_THREAD_DEFINE). Debe llamarse DESPUÉS de que la Wi-Fi tenga IP.
 *
 * Este hilo hace de "ISR de aplicación": escucha, acepta un cliente y, en
 * cuanto zsock_poll() indica datos disponibles, hace el trabajo MÍNIMO
 * (leerlos del socket y copiarlos a un buffer interno) antes de liberar
 * tcp_server_rx_ready. NO parsea JSON ni valida CRC — eso es responsabilidad
 * exclusiva de la tarea de procesamiento.
 */
void tcp_server_start(void);

/**
 * @brief Envía una trama ya construida (incluyendo el '\n' final) al
 *        cliente actualmente conectado, si lo hay. Usada por la tarea
 *        periódica de telemetría (telemetry_tx.c).
 *
 * @return true si había un cliente conectado y se intentó el envío;
 *         false si no hay ningún cliente conectado en este momento.
 */
bool tcp_server_send(const char *frame, size_t len);

/**
 * @brief Retira hasta dst_max bytes ya recibidos (y aún no consumidos)
 *        del buffer interno, en orden FIFO.
 *
 * Pensada para ser llamada EN BUCLE por la tarea de procesamiento
 * (rx_processor.c) tras despertar de tcp_server_rx_ready, hasta que
 * devuelva 0 (buffer vaciado). Es la única forma en que otro módulo toca
 * los datos crudos del socket: el acceso concurrente con el hilo
 * notificador está protegido internamente por un mutex, y la sección
 * crítica se mantiene deliberadamente corta.
 *
 * @return Número de bytes copiados a dst (puede ser 0).
 */
size_t tcp_server_drain_rx(char *dst, size_t dst_max);

#endif /* TCP_SERVER_H_ */
