#ifndef PROJECT_CONFIG_H_
#define PROJECT_CONFIG_H_

/*
 * Configuración que normalmente vas a modificar.
 *
 * ESP32 funciona como cliente TCP y se conecta al PC.
 * El PC ejecuta un servidor TCP en LISTEN_IP/LISTEN_PORT.
 */

#define WIFI_SSID       "TU_SSID"
#define WIFI_PASSWORD   "TU_PASSWORD"

/* Dirección IPv4 del PC en la misma red Wi-Fi que la ESP32. */
#define PC_SERVER_IP     "192.168.1.100"
#define PC_SERVER_PORT   5000

/* Tiempo entre telemetrías enviadas por la ESP32. */
#define TX_PERIOD_MS     2000

/* Tamaño máximo de una trama JSON completa. */
#define MAX_FRAME_SIZE   256

/* Número de tramas encolables antes de que la cola se llene. */
#define RX_QUEUE_LENGTH  8

/* Seguridad Wi-Fi. Para WPA/WPA2-PSK se usa WIFI_SECURITY_TYPE_PSK. */
#define WIFI_SECURITY    WIFI_SECURITY_TYPE_PSK

#endif /* PROJECT_CONFIG_H_ */
