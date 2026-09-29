#ifndef NETWORK_CONFIG_H_
#define NETWORK_CONFIG_H_

/*
 * network_config.h
 * =================
 * POR QUÉ este archivo existe: agrupa en un único lugar, con nombres
 * simples de encontrar, los valores que alguien SIN experiencia en Zephyr
 * necesita tocar para adaptar el proyecto a su propia red (SSID,
 * contraseña, puerto). Antes estos valores vivían en Kconfig/prj.conf,
 * lo que obliga a conocer el sistema de configuración de Zephyr; ahora
 * basta con editar las 4 líneas de abajo y recompilar. main.c incluye
 * este header directamente, y wifi_manager.c / tcp_server.c / 
 * telemetry_tx.c hacen lo mismo para leer estas mismas constantes: es
 * la "biblioteca asignada" a la configuración de red del proyecto.
 *
 * CON QUÉ se comunica: estos valores se usan al construir los parámetros
 * de conexión Wi-Fi (wifi_manager.c -> net_mgmt/NET_REQUEST_WIFI_CONNECT),
 * al abrir el socket servidor (tcp_server.c -> bind()) y al fijar el
 * período del bucle de la tarea de telemetría (telemetry_tx.c -> k_msleep()).
 */

/* SSID (nombre) de la red Wi-Fi 2.4 GHz a la que se conectará la ESP32.
 * PARÁMETRO: es un string, porque así lo exige wifi_connect_req_params.ssid
 * (arreglo de bytes + longitud). Reemplázalo por el SSID real de tu router. */
#define WIFI_SSID       "MI_RED_WIFI"

/* Contraseña (PSK) de la red Wi-Fi. PARÁMETRO: string en texto plano
 * porque wifi_connect_req_params.psk espera la passphrase WPA2 sin
 * procesar (Zephyr deriva la clave real internamente). Evita dejar
 * credenciales reales en un repositorio versionado. */
#define WIFI_PASSWORD   "MI_CONTRASENA"

/* Puerto TCP en el que la ESP32 escucha como SERVIDOR. PARÁMETRO: 5000
 * es un puerto alto, fuera del rango "well-known" (0-1023), por lo que
 * no requiere privilegios especiales y es poco probable que choque con
 * otro servicio. Debe coincidir con el puerto que uses al ejecutar
 * pc_client/main.py en la PC. */
#define TCP_SERVER_PORT 5000

/* Período (en milisegundos) de la tarea periódica de telemetría.
 * PARÁMETRO: 2000 ms es un valor cómodo para observar cambios en la GUI
 * de Python a simple vista sin saturar la red ni el log serial; bájalo
 * para más frecuencia de muestreo, súbelo para reducir tráfico. */
#define TELEMETRY_PERIOD_MS 2000

#endif /* NETWORK_CONFIG_H_ */
