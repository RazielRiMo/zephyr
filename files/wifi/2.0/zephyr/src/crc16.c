#include "crc16.h"

uint16_t crc16_ccitt_false(const uint8_t *data, size_t len)
{
	/* Semilla inicial 0xFFFF: parte fija del algoritmo CRC-16/CCITT-FALSE
	 * (no es arbitraria -- cambiar este valor da un CRC de una variante
	 * DISTINTA, incompatible con protocol.py). */
	uint16_t crc = 0xFFFF;

	for (size_t i = 0; i < len; i++) {
		/* Mezcla el byte actual en los 8 bits altos del registro de
		 * CRC. Es el punto donde cada byte del mensaje "entra" al
		 * cálculo. */
		crc ^= (uint16_t)data[i] << 8;

		/* Procesa el byte bit a bit (MSB primero, por eso "sin
		 * reflejar"): 8 iteraciones porque un byte tiene 8 bits. */
		for (int bit = 0; bit < 8; bit++) {
			if (crc & 0x8000) {
				/* Si el bit más significativo es 1, se
				 * desplaza y se aplica XOR con el polinomio
				 * generador 0x1021 (x^16+x^12+x^5+1 en
				 * notación polinómica) -- este es el corazón
				 * matemático del CRC-CCITT. */
				crc = (uint16_t)((crc << 1) ^ 0x1021);
			} else {
				crc = (uint16_t)(crc << 1);
			}
		}
	}

	return crc;
}
