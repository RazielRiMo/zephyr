#ifndef WIFI_MANAGER_H_
#define WIFI_MANAGER_H_

/**
 * @brief Conecta a la red Wi-Fi configurada en network_config.h
 *        (WIFI_SSID / WIFI_PASSWORD) y bloquea el hilo llamador hasta
 *        obtener una dirección IPv4 por DHCP (o hasta agotar el tiempo
 *        de espera).
 *
 * COMUNICACIÓN: se apoya en callbacks asíncronos del stack de red de
 * Zephyr (net_mgmt) para reaccionar a los eventos de asociación Wi-Fi y
 * de asignación de IP, en vez de sondear ("polling") el estado en un
 * bucle -- ver el detalle en wifi_manager.c.
 *
 * @return 0 en éxito; código de error negativo (errno.h) en caso contrario.
 */
int wifi_manager_connect(void);

#endif /* WIFI_MANAGER_H_ */
