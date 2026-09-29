#ifndef JSON_CONFIG_H_
#define JSON_CONFIG_H_

#include <zephyr/data/json.h>
#include <stdint.h>
#include <stdbool.h>

#define JSON_FRAME_MAX_LEN 256

/* =========================================================================
 * ZONA ALTAMENTE MODIFICABLE: REUTILIZA ESTE ARCHIVO EN CUALQUIER PROYECTO
 * =========================================================================
 * Para añadir un nuevo sensor o variable, sigue 3 pasos:
 * 1. Añade tu variable `int nueva_var;` a la estructura (tx o rx).
 * 2. Añádela al arreglo de descriptores correspondiente.
 * 3. Añádela a la función de cálculo de CRC con un XOR (^).
 */

/* --- 1. ESTRUCTURAS DE DATOS --- */

// Datos que el ESP32 ENVÍA al PC
struct datos_tx {
	int id_dispositivo;
	int temperatura;
	int humedad;
	int crc; // IMPORTANTE: El CRC siempre debe ir
};

// Datos que el ESP32 RECIBE del PC
struct datos_rx {
	int comando_led;
	int setpoint_temp;
	int crc; // IMPORTANTE: El CRC siempre debe ir
};

/* --- 2. DESCRIPTORES JSON (Mapeo de Variables C <-> JSON Keys) --- */

static const struct json_obj_descr tx_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct datos_tx, id_dispositivo, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct datos_tx, temperatura, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct datos_tx, humedad, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct datos_tx, crc, JSON_TOK_NUMBER),
};
static const size_t tx_descr_len = ARRAY_SIZE(tx_descr);

static const struct json_obj_descr rx_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct datos_rx, comando_led, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct datos_rx, setpoint_temp, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct datos_rx, crc, JSON_TOK_NUMBER),
};
static const size_t rx_descr_len = ARRAY_SIZE(rx_descr);

/* --- 3. CÁLCULO DE CRC (Algoritmo XOR acumulativo) --- */

// 1. Comando: calculate_crc_tx
// El porqué: Genera una firma matemática basada exclusivamente en las variables útiles, permitiendo que la codificación JSON y el orden de los campos no rompan la validación.
// La comunicación: Sirve para confirmar integridad hacia Python.
// Los parámetros: Un puntero a la estructura `datos_tx` que contiene los valores a validar.
static inline int calculate_crc_tx(struct datos_tx *d) {
    // Aplica XOR a todas tus variables para la firma
    return (d->id_dispositivo ^ d->temperatura ^ d->humedad) & 0xFFFF;
}

static inline int calculate_crc_rx(struct datos_rx *d) {
    // Aplica XOR a todas las variables recibidas
    return (d->comando_led ^ d->setpoint_temp) & 0xFFFF;
}

#endif /* JSON_CONFIG_H_ */
