/**
 * @file bmp280.h
 * @brief Librería portable para el sensor Bosch BMP280 (presión y
 *        temperatura) sobre bus I2C, usando exclusivamente las APIs
 *        estándar de Zephyr RTOS (zephyr/drivers/i2c.h).
 *
 * No depende de ningún HAL de fabricante. La dirección y el bus I2C
 * se obtienen del DeviceTree (ver macro BMP280_DEVICE_DT_STATIC_INIT);
 * nunca están codificados en este archivo.
 */

#ifndef BMP280_LIB_H_
#define BMP280_LIB_H_

#include <zephyr/drivers/i2c.h>
#include <zephyr/devicetree.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Instancia de un sensor BMP280.
 *
 * Se rellena inicialmente solo el campo `i2c` (con
 * BMP280_DEVICE_DT_STATIC_INIT o a mano); el resto lo completa
 * bmp280_init() al leer los coeficientes de calibración del sensor.
 */
struct bmp280_device {
	struct i2c_dt_spec i2c;

	/* Coeficientes de calibración de temperatura (registros 0x88-0x8D) */
	uint16_t dig_T1;
	int16_t  dig_T2;
	int16_t  dig_T3;

	/* Coeficientes de calibración de presión (registros 0x8E-0x9F) */
	uint16_t dig_P1;
	int16_t  dig_P2;
	int16_t  dig_P3;
	int16_t  dig_P4;
	int16_t  dig_P5;
	int16_t  dig_P6;
	int16_t  dig_P7;
	int16_t  dig_P8;
	int16_t  dig_P9;

	/* Valor intermedio de compensación, requerido para calcular presión */
	int32_t t_fine;

	/* Último código de error Zephyr (0 = sin error) */
	int last_error;

	/* true una vez bmp280_init() finalizó correctamente */
	bool ready;
};

/**
 * @brief Inicializa un struct bmp280_device a partir de un nodo DeviceTree.
 *
 * Uso típico:
 * @code
 * static struct bmp280_device bmp =
 *         BMP280_DEVICE_DT_STATIC_INIT(DT_NODELABEL(bmp280));
 * @endcode
 *
 * @param node_id Identificador de nodo DeviceTree (p. ej. DT_NODELABEL(bmp280))
 */
#define BMP280_DEVICE_DT_STATIC_INIT(node_id)   \
	{                                        \
		.i2c = I2C_DT_SPEC_GET(node_id), \
		.ready = false,                  \
		.last_error = 0,                 \
	}

/**
 * @brief Inicializa el sensor BMP280: verifica el bus, el chip ID,
 *        lee los coeficientes de calibración y configura el modo de
 *        medición (oversampling + filtro).
 *
 * @param dev Puntero a la instancia (con el campo .i2c ya definido).
 *
 * @retval 0        Éxito.
 * @retval -EINVAL  Puntero NULL.
 * @retval -ENODEV  Bus I2C no listo, o chip ID distinto al esperado
 *                  (dirección/cableado incorrectos).
 * @retval -EIO     Fallo de comunicación I2C (NACK, timeout del
 *                  controlador, etc.).
 */
int bmp280_init(struct bmp280_device *dev);

/**
 * @brief Lee la temperatura compensada.
 *
 * @param dev Instancia previamente inicializada con bmp280_init().
 * @return Temperatura en grados Celsius (2 decimales de resolución),
 *         o NAN si ocurrió un error (consultar bmp280_get_last_error()).
 */
float bmp280_read_temperature(struct bmp280_device *dev);

/**
 * @brief Lee la presión atmosférica compensada.
 *
 * @param dev Instancia previamente inicializada con bmp280_init().
 * @return Presión en hPa (2 decimales de resolución),
 *         o NAN si ocurrió un error (consultar bmp280_get_last_error()).
 */
float bmp280_read_pressure(struct bmp280_device *dev);

/**
 * @brief Lee temperatura y presión en una sola transacción I2C.
 *
 * Más eficiente que llamar a ambas funciones por separado cuando
 * necesitas los dos valores. Devuelve el código de error Zephyr
 * directamente (a diferencia de las funciones float).
 *
 * @param dev         Instancia previamente inicializada.
 * @param temperature [out] Temperatura en °C.
 * @param pressure    [out] Presión en hPa.
 * @return 0 en éxito, o un código de error negativo de Zephyr.
 */
int bmp280_read(struct bmp280_device *dev, float *temperature, float *pressure);

/**
 * @brief Devuelve el último código de error registrado por la instancia.
 */
int bmp280_get_last_error(const struct bmp280_device *dev);

/**
 * @brief Traduce un código de error de la librería a una descripción legible.
 */
const char *bmp280_error_str(int err);

#ifdef __cplusplus
}
#endif

#endif /* BMP280_LIB_H_ */
