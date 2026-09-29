#ifndef CRC16_H_
#define CRC16_H_

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Calcula el CRC-16/CCITT-FALSE de un bloque de datos.
 *
 * Implementación manual, bit a bit (no depende de zephyr/sys/crc.h), para
 * poder documentar y replicar EXACTAMENTE el mismo algoritmo en el lado
 * PC (ver protocol.py). Parámetros del algoritmo:
 *
 *   - Polinomio:       0x1021
 *   - Valor inicial:   0xFFFF
 *   - Reflejado (in):  No
 *   - Reflejado (out): No
 *   - XOR de salida:   0x0000
 *
 * Vector de prueba estándar para validar cualquier reimplementación:
 *   crc16_ccitt_false((uint8_t *)"123456789", 9) == 0x29B1
 *
 * @param data Puntero al buffer de datos.
 * @param len  Longitud en bytes del buffer.
 * @return Valor de 16 bits del CRC calculado.
 */
uint16_t crc16_ccitt_false(const uint8_t *data, size_t len);

#endif /* CRC16_H_ */
