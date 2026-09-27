#ifndef TM1638_H_
#define TM1638_H_

#include <stdint.h>
#include <zephyr/drivers/gpio.h>

/*
 * Libreria simple para el modulo TM1638 (8 displays de 7 segmentos,
 * 8 LEDs, 8 botones) sobre Zephyr RTOS, implementada con la API de
 * GPIO de Zephyr (sin nada de Arduino/HAL/delay()/digitalWrite()).
 *
 * Es una libreria "de un solo dispositivo": guarda su estado en
 * variables estaticas dentro de tm1638.c, asi que ninguna funcion
 * recibe ni devuelve un handle/puntero al dispositivo -- eso es lo
 * que permite usarla como tm1638_display(numero) directamente desde
 * main.c. La limitacion a cambio es que solo soporta un TM1638 activo
 * a la vez (de sobra para el caso tipico de un solo modulo).
 *
 * Codigos de retorno usados por las funciones que pueden fallar:
 *    0        -> exito
 *   -EINVAL   -> argumento fuera de rango
 *   -EIO      -> error de comunicacion o libreria no inicializada
 *   -ENODEV   -> (solo en tm1638_init) un GPIO del devicetree no esta listo
 */

/**
 * Inicializa el TM1638 con los 3 pines indicados. Deja el modulo con
 * todos los digitos y LEDs apagados y brillo medio (nivel 4).
 *
 * Los gpio_dt_spec normalmente se obtienen del devicetree con
 * GPIO_DT_SPEC_GET() en main.c (ver ejemplo en main.c y app.overlay).
 */
int tm1638_init(struct gpio_dt_spec stb, struct gpio_dt_spec clk, struct gpio_dt_spec dio);

/**
 * Ajusta el brillo de la pantalla.
 * level: 0 (minimo) a 7 (maximo). Fuera de ese rango -> -EINVAL.
 */
int tm1638_set_brightness(uint8_t level);

/**
 * Muestra un numero entero en los 8 digitos de 7 segmentos, alineado a
 * la derecha (el digito de la derecha = unidades, igual que en una
 * calculadora). Los digitos no usados quedan apagados. value = 0 se
 * muestra correctamente como "0" en el digito de la derecha.
 *
 * Rango valido: 0 a 99999999 (8 digitos, el maximo que entra en el
 * modulo). Un valor fuera de ese rango (incluye negativos) no se puede
 * representar con 8 digitos: la funcion devuelve -EINVAL y, para que
 * el error tambien sea visible en el propio modulo, enciende una fila
 * de guiones "--------" en vez de dejar el valor anterior sin avisar.
 */
int tm1638_display(int value);

/**
 * Control manual de los segmentos de un digito especifico (para
 * dibujar letras, simbolos, o cualquier patron que no sea 0-9).
 *
 * digit: 0-7 (0 = digito mas a la izquierda, 7 = mas a la derecha).
 *        Fuera de rango -> -EINVAL.
 * segments: mapa de bits, uno por segmento -- este es el orden que usa
 *           el TM1638 en su registro de display, no hace falta
 *           reordenarlo:
 *     bit 0 = a      bit 4 = e
 *     bit 1 = b      bit 5 = f
 *     bit 2 = c      bit 6 = g
 *     bit 3 = d      bit 7 = dp (punto decimal)
 *           0xFF enciende los 7 segmentos + el punto decimal.
 *           0x00 apaga todos los segmentos de ese digito.
 */
int tm1638_set_digit(int digit, uint8_t segments);

/** Apaga los 8 digitos de 7 segmentos Y los 8 LEDs. */
int tm1638_clear(void);

/** Apaga solo los 8 digitos de 7 segmentos (los LEDs no se tocan). */
int tm1638_clear_digits(void);

/** Apaga solo los 8 LEDs (los digitos de 7 segmentos no se tocan). */
int tm1638_clear_leds(void);

/**
 * Enciende o apaga un LED individual.
 * led: 0-7. state: 0 (apagado) o 1 (encendido).
 * Cualquier valor fuera de esos rangos devuelve -EINVAL.
 */
int tm1638_set_led(int led, int state);

/**
 * Devuelve que boton esta presionado, con antirrebote (debounce) ya
 * aplicado:
 *    0     -> ningun boton presionado
 *   1..8   -> boton N presionado (numerados de izquierda a derecha)
 *   -EIO   -> libreria no inicializada
 *
 * Si se presiona mas de un boton a la vez, se devuelve el de menor
 * numero (el mas a la izquierda). Es una decision de diseno necesaria
 * porque la funcion solo puede comunicar un boton por llamada; ver el
 * comentario junto a su implementacion en tm1638.c si tu proyecto
 * necesita conocer el estado de los 8 botones simultaneamente.
 *
 * El antirrebote es NO bloqueante (no llama a k_sleep internamente):
 * una lectura solo se toma como valida despues de que el estado fisico
 * se mantiene estable durante TM1638_DEBOUNCE_MS. Por eso hay que
 * llamar a esta funcion periodicamente (p. ej. cada 10-20 ms dentro
 * del loop principal); una sola llamada aislada puede no reflejar
 * todavia el estado "asentado" del boton.
 */
int tm1638_get_button(void);

#endif /* TM1638_H_ */
