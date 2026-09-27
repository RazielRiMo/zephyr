/*
 * Ejemplo de uso de la libreria bmp280 (I2C) en Zephyr RTOS.
 * Muestra: inicializacion, verificacion de errores y lectura
 * periodica de temperatura y presion.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include "bmp280.h"

/* El nodo "bmp280" viene de boards/<placa>.overlay.
 * La libreria no fija ninguna direccion; toda la informacion I2C
 * sale de este spec construido a partir del DeviceTree. */
static struct bmp280_device bmp = BMP280_DEVICE_DT_STATIC_INIT(DT_NODELABEL(bmp280));

int main(void)
{
	int ret = bmp280_init(&bmp);

	if (ret < 0) {
		printk("Error al inicializar el BMP280 (%d): %s\n",
		       ret, bmp280_error_str(ret));
		return ret;
	}

	printk("BMP280 inicializado correctamente\n");

	while (1) {
		float temperature = bmp280_read_temperature(&bmp);
		float pressure = bmp280_read_pressure(&bmp);
		int err = bmp280_get_last_error(&bmp);

		if (err != 0) {
			printk("Error de lectura (%d): %s\n", err, bmp280_error_str(err));
		} else {
			printk("Temperatura: %.2f C\n", (double)temperature);
			printk("Presion: %.2f hPa\n", (double)pressure);
		}

		k_sleep(K_SECONDS(1));
	}

	return 0;
}
