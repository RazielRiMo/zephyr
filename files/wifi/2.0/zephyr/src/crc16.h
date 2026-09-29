#ifndef CRC16_H_
#define CRC16_H_

#include <stddef.h>
#include <stdint.h>

/*
 * crc16.h / crc16.c
 * ==================
 * POR QUÉ: es el mecanismo de detección de integridad que exige el
 * enunciado. Cada trama JSON (en ambas direcciones) viaja acompañada de
 * un CRC-16 calculado sobre sus bytes de datos; si esos bytes se alteran
 * en tránsito (ruido de red, trama recortada, etc.), el CRC recalculado
 * en el receptor no coincidirá con el recibido, y eso es lo que dispara
 * el conteo de errores y el aviso por consola/serial.
 *
 * CON QUÉ se comunica: no depende de sockets, hilos ni JSON -- es una
 * función matemática pura (bytes de entrada -> entero de 16 bits). La usa
 * json_protocol.c, tanto al construir tramas salientes (telemetry_tx.c)
 * como al verificar tramas entrantes (rx_processor.c).
 */

/**
 * @brief Calcula el CRC-16/CCITT-FALSE de un bloque de datos.
 *
 * Implementación manual, bit a bit (no depende de zephyr/sys/crc.h), para
 * poder documentar y replicar EXACTAMENTE el mismo algoritmo en el lado
 * PC (ver protocol.py) sin ambigüedad sobre qué variante de "CRC-16" se
 * está usando (existen muchas: CCITT, XMODEM, MODBUS, etc., cada una con
 * distinto polinomio/semilla/reflejo).
 *
 * Parámetros fijos del algoritmo (deben coincidir en C y en Python):
 *   - Polinomio:       0x1021
 *   - Valor inicial:   0xFFFF
 *   - Reflejado (in):  No
 *   - Reflejado (out): No
 *   - XOR de salida:   0x0000
 *
 * Vector de prueba estándar para validar cualquier reimplementación:
 *   crc16_ccitt_false((uint8_t *)"123456789", 9) == 0x29B1
 *
 * @param data PARÁMETRO: puntero al primer byte a proteger. Es
 *             "const uint8_t *" (no "char *") para dejar explícito que la
 *             función trata la entrada como bytes crudos, no como texto.
 * @param len  PARÁMETRO: cuántos bytes de 'data' incluir en el cálculo.
 *             Se pasa explícito (en vez de asumir una cadena terminada en
 *             '\0') porque el buffer JSON ya codificado por
 *             json_obj_encode_buf() SÍ está NUL-terminado, pero mantener
 *             'len' explícito hace la función reutilizable también sobre
 *             datos binarios sin terminador.
 * @return Valor de 16 bits del CRC calculado.
 */
uint16_t crc16_ccitt_false(const uint8_t *data, size_t len);

#endif /* CRC16_H_ */
