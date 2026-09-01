/**
 * @file bmp280.c
 * @brief Implementación de la librería BMP280 para Zephyr RTOS (I2C).
 *
 * Referencias de registro y fórmulas de compensación: hoja de datos
 * oficial Bosch BMP280 (BST-BMP280-DS001), sección "Compensation
 * formulas" (enteros de 32/64 bits, sin punto flotante en el sensor).
 */

#include "bmp280.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <errno.h>
#include <math.h>

LOG_MODULE_REGISTER(bmp280, LOG_LEVEL_INF);

/* ---- Mapa de registros BMP280 ------------------------------------- */
#define BMP280_REG_CALIB_START   0x88
#define BMP280_REG_CALIB_LEN     24   /* dig_T1..dig_P9, 12 x 16 bits */

#define BMP280_REG_CHIP_ID       0xD0
#define BMP280_CHIP_ID_VALUE     0x58

#define BMP280_REG_RESET         0xE0
#define BMP280_RESET_VALUE       0xB6

#define BMP280_REG_STATUS        0xF3
#define BMP280_STATUS_IM_UPDATE  BIT(0) /* 1 = aún copiando NVM->registros */

#define BMP280_REG_CTRL_MEAS     0xF4
#define BMP280_REG_CONFIG        0xF5

#define BMP280_REG_PRESS_MSB     0xF7
#define BMP280_RAW_DATA_LEN      6      /* press_msb..temp_xlsb, contiguos */

/* ---- Configuración de medición ------------------------------------
 * Perfil "indoor navigation" recomendado por Bosch: buena resolución
 * de presión (para altímetro/detección de movimiento vertical) con
 * temperatura suficiente para compensar correctamente, y filtro IIR
 * para suavizar el ruido de presión sin retardo excesivo.
 *   osrs_t = x2, osrs_p = x16, modo normal, filtro IIR = 16, standby 0.5 ms
 */
#define BMP280_OSRS_T_X2         (0x02 << 5)
#define BMP280_OSRS_P_X16        (0x05 << 2)
#define BMP280_MODE_NORMAL       (0x03)
#define BMP280_CTRL_MEAS_VALUE   (BMP280_OSRS_T_X2 | BMP280_OSRS_P_X16 | BMP280_MODE_NORMAL)

#define BMP280_FILTER_X16        (0x04 << 2)
#define BMP280_STANDBY_0_5MS     (0x00 << 5)
#define BMP280_CONFIG_VALUE      (BMP280_STANDBY_0_5MS | BMP280_FILTER_X16)

/* --------------------------------------------------------------------
 * Lectura cruda de los 6 registros de datos (presión + temperatura)
 * en una sola ráfaga I2C, tal como recomienda el datasheet para
 * garantizar consistencia entre ambas medidas.
 * ------------------------------------------------------------------ */
static int bmp280_read_raw(struct bmp280_device *dev, int32_t *adc_T, int32_t *adc_P)
{
	uint8_t buf[BMP280_RAW_DATA_LEN];
	int ret;

	if (!dev->ready) {
		return -EACCES;
	}

	ret = i2c_burst_read_dt(&dev->i2c, BMP280_REG_PRESS_MSB, buf, sizeof(buf));
	if (ret < 0) {
		LOG_ERR("Fallo leyendo registros de datos 0x%02X (err %d)",
			BMP280_REG_PRESS_MSB, ret);
		return ret;
	}

	*adc_P = ((int32_t)buf[0] << 12) | ((int32_t)buf[1] << 4) | ((int32_t)buf[2] >> 4);
	*adc_T = ((int32_t)buf[3] << 12) | ((int32_t)buf[4] << 4) | ((int32_t)buf[5] >> 4);

	return 0;
}

/* Fórmula oficial de compensación de temperatura (datasheet Bosch,
 * versión de 32 bits). Actualiza dev->t_fine (necesario para presión)
 * y devuelve la temperatura en centésimas de grado Celsius. */
static int32_t bmp280_compensate_temperature(struct bmp280_device *dev, int32_t adc_T)
{
	int32_t var1, var2;

	var1 = ((((adc_T >> 3) - ((int32_t)dev->dig_T1 << 1))) * ((int32_t)dev->dig_T2)) >> 11;
	var2 = (((((adc_T >> 4) - ((int32_t)dev->dig_T1)) *
		  ((adc_T >> 4) - ((int32_t)dev->dig_T1))) >> 12) *
		((int32_t)dev->dig_T3)) >> 14;

	dev->t_fine = var1 + var2;

	return (dev->t_fine * 5 + 128) >> 8; /* en centi-°C */
}

/* Fórmula oficial de compensación de presión (datasheet Bosch,
 * versión de 64 bits). Requiere dev->t_fine ya actualizado.
 * Devuelve la presión en formato Q24.8 (Pa * 256). */
static uint32_t bmp280_compensate_pressure(struct bmp280_device *dev, int32_t adc_P)
{
	int64_t var1, var2, p;

	var1 = ((int64_t)dev->t_fine) - 128000;
	var2 = var1 * var1 * (int64_t)dev->dig_P6;
	var2 = var2 + ((var1 * (int64_t)dev->dig_P5) << 17);
	var2 = var2 + (((int64_t)dev->dig_P4) << 35);
	var1 = ((var1 * var1 * (int64_t)dev->dig_P3) >> 8) +
	       ((var1 * (int64_t)dev->dig_P2) << 12);
	var1 = (((((int64_t)1) << 47) + var1)) * ((int64_t)dev->dig_P1) >> 33;

	if (var1 == 0) {
		return 0; /* evita división por cero (ver datasheet) */
	}

	p = 1048576 - adc_P;
	p = (((p << 31) - var2) * 3125) / var1;
	var1 = (((int64_t)dev->dig_P9) * (p >> 13) * (p >> 13)) >> 25;
	var2 = (((int64_t)dev->dig_P8) * p) >> 19;
	p = ((p + var1 + var2) >> 8) + (((int64_t)dev->dig_P7) << 4);

	return (uint32_t)p;
}

int bmp280_init(struct bmp280_device *dev)
{
	uint8_t chip_id;
	uint8_t status;
	uint8_t calib[BMP280_REG_CALIB_LEN];
	int ret;

	if (dev == NULL) {
		return -EINVAL;
	}

	dev->ready = false;

	/* 1) El controlador I2C subyacente debe estar listo */
	if (!i2c_is_ready_dt(&dev->i2c)) {
		LOG_ERR("Bus I2C no disponible para el dispositivo BMP280");
		dev->last_error = -ENODEV;
		return -ENODEV;
	}

	/* 2) Reset por software: parte de un estado conocido */
	ret = i2c_reg_write_byte_dt(&dev->i2c, BMP280_REG_RESET, BMP280_RESET_VALUE);
	if (ret < 0) {
		LOG_ERR("Sin respuesta en direccion I2C 0x%02X (err %d): "
			"revise cableado/direccion", dev->i2c.addr, ret);
		dev->last_error = ret;
		return ret;
	}
	k_msleep(5);

	/* 3) Verificar chip ID */
	ret = i2c_reg_read_byte_dt(&dev->i2c, BMP280_REG_CHIP_ID, &chip_id);
	if (ret < 0) {
		LOG_ERR("Error leyendo chip ID (err %d)", ret);
		dev->last_error = ret;
		return ret;
	}
	if (chip_id != BMP280_CHIP_ID_VALUE) {
		LOG_ERR("Chip ID inesperado: 0x%02X (se esperaba 0x%02X)",
			chip_id, BMP280_CHIP_ID_VALUE);
		dev->last_error = -ENODEV;
		return -ENODEV;
	}

	/* 4) Esperar a que el sensor termine de copiar calibración de NVM */
	for (int i = 0; i < 10; i++) {
		ret = i2c_reg_read_byte_dt(&dev->i2c, BMP280_REG_STATUS, &status);
		if (ret < 0) {
			dev->last_error = ret;
			return ret;
		}
		if (!(status & BMP280_STATUS_IM_UPDATE)) {
			break;
		}
		k_msleep(2);
	}

	/* 5) Leer los 24 bytes de coeficientes de calibración (0x88-0x9F) */
	ret = i2c_burst_read_dt(&dev->i2c, BMP280_REG_CALIB_START, calib, sizeof(calib));
	if (ret < 0) {
		LOG_ERR("Error leyendo coeficientes de calibracion (err %d)", ret);
		dev->last_error = ret;
		return ret;
	}

	dev->dig_T1 = (uint16_t)(calib[0]  | (calib[1]  << 8));
	dev->dig_T2 = (int16_t)(calib[2]  | (calib[3]  << 8));
	dev->dig_T3 = (int16_t)(calib[4]  | (calib[5]  << 8));
	dev->dig_P1 = (uint16_t)(calib[6]  | (calib[7]  << 8));
	dev->dig_P2 = (int16_t)(calib[8]  | (calib[9]  << 8));
	dev->dig_P3 = (int16_t)(calib[10] | (calib[11] << 8));
	dev->dig_P4 = (int16_t)(calib[12] | (calib[13] << 8));
	dev->dig_P5 = (int16_t)(calib[14] | (calib[15] << 8));
	dev->dig_P6 = (int16_t)(calib[16] | (calib[17] << 8));
	dev->dig_P7 = (int16_t)(calib[18] | (calib[19] << 8));
	dev->dig_P8 = (int16_t)(calib[20] | (calib[21] << 8));
	dev->dig_P9 = (int16_t)(calib[22] | (calib[23] << 8));

	/* 6) Configurar filtro/standby y luego oversampling + modo normal */
	ret = i2c_reg_write_byte_dt(&dev->i2c, BMP280_REG_CONFIG, BMP280_CONFIG_VALUE);
	if (ret < 0) {
		dev->last_error = ret;
		return ret;
	}
	ret = i2c_reg_write_byte_dt(&dev->i2c, BMP280_REG_CTRL_MEAS, BMP280_CTRL_MEAS_VALUE);
	if (ret < 0) {
		dev->last_error = ret;
		return ret;
	}

	dev->t_fine = 0;
	dev->last_error = 0;
	dev->ready = true;

	LOG_INF("BMP280 listo en direccion I2C 0x%02X", dev->i2c.addr);

	return 0;
}

float bmp280_read_temperature(struct bmp280_device *dev)
{
	int32_t adc_T, adc_P;
	int32_t T;

	if (dev == NULL) {
		return NAN;
	}
	if (bmp280_read_raw(dev, &adc_T, &adc_P) < 0) {
		return NAN;
	}

	T = bmp280_compensate_temperature(dev, adc_T);
	dev->last_error = 0;

	return (float)T / 100.0f;
}

float bmp280_read_pressure(struct bmp280_device *dev)
{
	int32_t adc_T, adc_P;
	uint32_t P;
	int ret;

	if (dev == NULL) {
		return NAN;
	}

	ret = bmp280_read_raw(dev, &adc_T, &adc_P);
	if (ret < 0) {
		dev->last_error = ret;
		return NAN;
	}

	/* La presión depende de t_fine: siempre se compensa primero la
	 * temperatura, aunque el llamador solo quiera la presión. */
	(void)bmp280_compensate_temperature(dev, adc_T);
	P = bmp280_compensate_pressure(dev, adc_P);
	dev->last_error = 0;

	return (float)P / 256.0f / 100.0f; /* Q24.8 Pa -> hPa */
}

int bmp280_read(struct bmp280_device *dev, float *temperature, float *pressure)
{
	int32_t adc_T, adc_P;
	int32_t T;
	uint32_t P;
	int ret;

	if (dev == NULL || temperature == NULL || pressure == NULL) {
		return -EINVAL;
	}

	ret = bmp280_read_raw(dev, &adc_T, &adc_P);
	if (ret < 0) {
		dev->last_error = ret;
		return ret;
	}

	T = bmp280_compensate_temperature(dev, adc_T);
	P = bmp280_compensate_pressure(dev, adc_P);

	*temperature = (float)T / 100.0f;
	*pressure = (float)P / 256.0f / 100.0f;
	dev->last_error = 0;

	return 0;
}

int bmp280_get_last_error(const struct bmp280_device *dev)
{
	return dev ? dev->last_error : -EINVAL;
}

const char *bmp280_error_str(int err)
{
	switch (err) {
	case 0:
		return "Sin error";
	case -ENODEV:
		return "Sensor no encontrado o chip ID invalido: revise direccion I2C y cableado";
	case -EIO:
		return "Error de comunicacion I2C: bus ocupado, sensor desconectado o mal alimentado";
	case -EINVAL:
		return "Parametro invalido (puntero NULL)";
	case -EACCES:
		return "Sensor no inicializado: llame primero a bmp280_init()";
	case -ETIMEDOUT:
		return "Tiempo de espera agotado en la comunicacion I2C";
	default:
		return "Error desconocido";
	}
}
