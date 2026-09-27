#include "tm1638.h"
#include <zephyr/kernel.h>
#include <stdbool.h>
#include <errno.h>

#define TM1638_DEBOUNCE_MS 30
#define TM1638_MAX_VALUE   99999999

/*
 * --- Protocolo del TM1638 (resumen) ---
 *
 * 3 lineas:
 *   STB  chip select. En reposo esta en alto; se pone en bajo mientras
 *        dura una transaccion (uno o mas bytes) y vuelve a alto al
 *        terminarla.
 *   CLK  reloj generado por nosotros (el maestro). En reposo alto.
 *   DIO  datos, BIDIRECCIONAL: nosotros la manejamos como salida para
 *        escribir comandos/datos, y la reconfiguramos como entrada
 *        solo durante los 4 bytes que el TM1638 responde al leer el
 *        teclado (ver scan_keys_raw). Por eso cada funcion de bajo
 *        nivel reconfigura gpio_pin_configure_dt(&tm.dio, ...) segun
 *        si va a transmitir o a recibir.
 *
 * Cada byte va LSB primero; DIO cambia con CLK en bajo y se
 * muestrea/valida con CLK en alto (igual que un SPI modo 0 casero).
 *
 * Comandos que usa esta libreria (primer byte de cada transaccion, con
 * STB en bajo mientras dura):
 *   0x44        Modo de escritura con direccion fija: el siguiente
 *               byte fija la direccion (0xC0|addr) y el que sigue a
 *               ese es el dato para esa direccion. Se usa para poder
 *               escribir un solo digito/LED sin tocar el resto.
 *   0xC0 | addr Fija la direccion (0-15) de la memoria de display para
 *               el byte de datos que sigue.
 *   0x80 | n    Control de pantalla: bit3 = encendida (1) / apagada
 *               (0), bits 0-2 = brillo (0-7).
 *   0x42        Inicia lectura del teclado: tras enviar este byte con
 *               STB en bajo, el maestro pone DIO en entrada y hace 4
 *               pulsos de reloj mas para leer 4 bytes de vuelta.
 *
 * La memoria de display tiene 16 direcciones (0x0-0xF). En los modulos
 * "LED&KEY" (el mas comun: 8 digitos + 8 LEDs + 8 botones en fila),
 * cada digito i vive en la direccion par i*2 y su LED asociado en la
 * direccion impar i*2+1.
 */

struct tm1638_state {
	struct gpio_dt_spec stb;
	struct gpio_dt_spec clk;
	struct gpio_dt_spec dio;
	bool initialized;

	/* Antirrebote de botones (ver tm1638_get_button). */
	uint8_t keys_raw_prev;
	uint8_t keys_stable;
	int64_t keys_change_ts;
};

static struct tm1638_state tm;

/* Segmentos a-g para digitos 0-9, usados por tm1638_display(). */
static const uint8_t digit_font[10] = {
	0x3F, 0x06, 0x5B, 0x4F, 0x66,
	0x6D, 0x7D, 0x07, 0x7F, 0x6F,
};

static void send_byte(uint8_t b)
{
	gpio_pin_configure_dt(&tm.dio, GPIO_OUTPUT);

	for (int i = 0; i < 8; i++) {
		gpio_pin_set_dt(&tm.clk, 0);
		gpio_pin_set_dt(&tm.dio, (b >> i) & 0x01);
		k_busy_wait(1);
		gpio_pin_set_dt(&tm.clk, 1);
		k_busy_wait(1);
	}
}

static uint8_t recv_byte(void)
{
	uint8_t b = 0;

	gpio_pin_configure_dt(&tm.dio, GPIO_INPUT);

	for (int i = 0; i < 8; i++) {
		gpio_pin_set_dt(&tm.clk, 0);
		k_busy_wait(1);
		if (gpio_pin_get_dt(&tm.dio)) {
			b |= (1 << i);
		}
		gpio_pin_set_dt(&tm.clk, 1);
		k_busy_wait(1);
	}
	return b;
}

/* Escribe un byte en una direccion fija (0-15) de la memoria de display. */
static void write_at(uint8_t addr, uint8_t data)
{
	gpio_pin_set_dt(&tm.stb, 0);
	send_byte(0x44);
	gpio_pin_set_dt(&tm.stb, 1);
	k_busy_wait(1);

	gpio_pin_set_dt(&tm.stb, 0);
	send_byte(0xC0 | (addr & 0x0F));
	send_byte(data);
	gpio_pin_set_dt(&tm.stb, 1);
}

/* Lee el estado crudo (sin antirrebote) de los 8 botones. */
static uint8_t scan_keys_raw(void)
{
	uint8_t keys = 0;
	uint8_t buf[4];

	gpio_pin_set_dt(&tm.stb, 0);
	send_byte(0x42);
	for (int i = 0; i < 4; i++) {
		buf[i] = recv_byte();
	}
	gpio_pin_set_dt(&tm.stb, 1);
	gpio_pin_configure_dt(&tm.dio, GPIO_OUTPUT);

	for (int i = 0; i < 4; i++) {
		if (buf[i] & 0x01) {
			keys |= (1 << (i * 2));
		}
		if (buf[i] & 0x10) {
			keys |= (1 << (i * 2 + 1));
		}
	}
	return keys;
}

int tm1638_init(struct gpio_dt_spec stb, struct gpio_dt_spec clk, struct gpio_dt_spec dio)
{
	int ret;

	tm.stb = stb;
	tm.clk = clk;
	tm.dio = dio;
	tm.initialized = false;
	tm.keys_raw_prev = 0;
	tm.keys_stable = 0;
	tm.keys_change_ts = k_uptime_get();

	if (!gpio_is_ready_dt(&tm.stb) || !gpio_is_ready_dt(&tm.clk) ||
	    !gpio_is_ready_dt(&tm.dio)) {
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&tm.stb, GPIO_OUTPUT_ACTIVE);
	ret |= gpio_pin_configure_dt(&tm.clk, GPIO_OUTPUT_ACTIVE);
	ret |= gpio_pin_configure_dt(&tm.dio, GPIO_OUTPUT_ACTIVE);
	if (ret) {
		return -EIO;
	}

	tm.initialized = true;

	tm1638_clear();
	tm1638_set_brightness(4);
	return 0;
}

int tm1638_set_brightness(uint8_t level)
{
	if (!tm.initialized) {
		return -EIO;
	}
	if (level > 7) {
		return -EINVAL;
	}

	gpio_pin_set_dt(&tm.stb, 0);
	send_byte(0x88 | level); /* bit3=1 -> pantalla encendida */
	gpio_pin_set_dt(&tm.stb, 1);
	return 0;
}

int tm1638_set_digit(int digit, uint8_t segments)
{
	if (!tm.initialized) {
		return -EIO;
	}
	if (digit < 0 || digit > 7) {
		return -EINVAL;
	}

	write_at((uint8_t)digit * 2, segments);
	return 0;
}

int tm1638_display(int value)
{
	if (!tm.initialized) {
		return -EIO;
	}

	if (value < 0 || value > TM1638_MAX_VALUE) {
		/* No representable en 8 digitos: se deja claro con
		 * guiones en vez de fallar en silencio.
		 */
		for (int pos = 0; pos < 8; pos++) {
			tm1638_set_digit(pos, 0x40);
		}
		return -EINVAL;
	}

	tm1638_clear_digits();

	int pos = 7;
	uint32_t v = (uint32_t)value;

	do {
		tm1638_set_digit(pos, digit_font[v % 10]);
		v /= 10;
		pos--;
	} while (v > 0 && pos >= 0);

	return 0;
}

int tm1638_clear(void)
{
	if (!tm.initialized) {
		return -EIO;
	}
	for (uint8_t addr = 0; addr < 16; addr++) {
		write_at(addr, 0x00);
	}
	return 0;
}

int tm1638_clear_digits(void)
{
	if (!tm.initialized) {
		return -EIO;
	}
	for (uint8_t pos = 0; pos < 8; pos++) {
		write_at(pos * 2, 0x00);
	}
	return 0;
}

int tm1638_clear_leds(void)
{
	if (!tm.initialized) {
		return -EIO;
	}
	for (uint8_t pos = 0; pos < 8; pos++) {
		write_at(pos * 2 + 1, 0x00);
	}
	return 0;
}

int tm1638_set_led(int led, int state)
{
	if (!tm.initialized) {
		return -EIO;
	}
	if (led < 0 || led > 7) {
		return -EINVAL;
	}
	if (state != 0 && state != 1) {
		return -EINVAL;
	}

	write_at((uint8_t)led * 2 + 1, state ? 0x01 : 0x00);
	return 0;
}

int tm1638_get_button(void)
{
	if (!tm.initialized) {
		return -EIO;
	}

	uint8_t raw = scan_keys_raw();
	int64_t now = k_uptime_get();

	if (raw != tm.keys_raw_prev) {
		/* Cambio detectado: reinicia el temporizador de estabilidad.
		 * Todavia NO se acepta como valido -- podria ser rebote.
		 */
		tm.keys_raw_prev = raw;
		tm.keys_change_ts = now;
	} else if ((now - tm.keys_change_ts) >= TM1638_DEBOUNCE_MS) {
		/* El estado crudo lleva sin cambiar >= TM1638_DEBOUNCE_MS:
		 * ahora si se acepta como el estado "estable" real.
		 */
		tm.keys_stable = raw;
	}

	if (tm.keys_stable == 0) {
		return 0;
	}

	/* Varios botones a la vez: se prioriza el de menor numero. */
	for (int i = 0; i < 8; i++) {
		if (tm.keys_stable & BIT(i)) {
			return i + 1;
		}
	}

	return 0;
}
