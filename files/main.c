#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include "tm1638.h"

/*
 * Los 3 pines se definen en app.overlay dentro del nodo estandar
 * "zephyr,user" -- el patron recomendado por Zephyr para exponer GPIO
 * sueltos a la aplicacion sin escribir un devicetree binding propio.
 * Si cambias de pines fisicos, solo hay que editar app.overlay: este
 * archivo no cambia.
 */
static const struct gpio_dt_spec stb_pin =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), tm1638_stb_gpios);
static const struct gpio_dt_spec clk_pin =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), tm1638_clk_gpios);
static const struct gpio_dt_spec dio_pin =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), tm1638_dio_gpios);

int main(void)
{
	int ret;

	/* 1) Inicializar el TM1638 */
	ret = tm1638_init(stb_pin, clk_pin, dio_pin);
	if (ret != 0) {
		printk("Error al inicializar el TM1638 (ret=%d)\n", ret);
		return 0;
	}

	/* 2) tm1638_display(): mostrar un numero en los 8 digitos */
	tm1638_display(12345678);
	k_msleep(1500);

	/* 3) tm1638_set_digit(): control manual de un digito.
	 *    0x77 = segmentos a,b,c,e,f,g encendidos -> dibuja una "A". */
	tm1638_set_digit(2, 0x77);
	k_msleep(1000);

	/* 4) 0xFF enciende los 7 segmentos + el punto decimal del digito */
	tm1638_set_digit(0, 0xFF);
	k_msleep(1000);

	/* 5) tm1638_set_led(): encender y apagar LEDs individuales */
	for (int i = 0; i < 8; i++) {
		tm1638_set_led(i, 1);
		k_msleep(100);
	}
	k_msleep(500);
	for (int i = 0; i < 8; i++) {
		tm1638_set_led(i, 0);
		k_msleep(100);
	}

	/* 6) tm1638_clear_digits(): apaga solo los 7 segmentos, los LEDs
	 *    quedan como esten (se nota porque dejamos uno encendido). */
	tm1638_set_led(3, 1);
	tm1638_display(8888);
	k_msleep(800);
	tm1638_clear_digits();
	k_msleep(800);

	/* 7) tm1638_clear_leds(): apaga solo los LEDs */
	tm1638_clear_leds();
	k_msleep(500);

	/* 8) tm1638_clear(): apaga absolutamente todo */
	tm1638_display(1234);
	tm1638_set_led(5, 1);
	k_msleep(800);
	tm1638_clear();
	k_msleep(500);

	printk("Demo inicial terminada. Presiona los botones del modulo...\n");

	/*
	 * 9), 10) y 11): loop principal.
	 *
	 * tm1638_get_button() ya aplica antirrebote internamente, pero
	 * necesita que se le llame de forma periodica para que ese
	 * antirrebote pueda "asentarse" (ver tm1638.c). Sondeamos cada
	 * 20 ms con k_msleep(), que cede la CPU al scheduler en vez de
	 * hacer busy-wait -- el nucleo puede atender otros hilos o entrar
	 * en idle durante esa espera, no se desperdicia CPU.
	 */
	int last_button = 0;
	int counter = 0;

	while (1) {
		int button = tm1638_get_button();

		/* Solo reaccionamos en el flanco de "recien presionado",
		 * no en cada iteracion mientras se mantiene presionado.
		 */
		if (button > 0 && button != last_button) {
			printk("Boton %d presionado\n", button);
			counter++;
			tm1638_display(counter);
		}
		last_button = button;

		k_msleep(20);
	}

	return 0;
}
