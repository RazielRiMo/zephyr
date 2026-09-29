/*
 * wifi_manager.c
 * ===============
 * POR QUÉ este módulo existe: separa "cómo se conecta la ESP32 a una red
 * Wi-Fi y espera una IP" de todo lo demás (sockets, JSON, hilos de la
 * aplicación). main.c llama a una sola función bloqueante
 * (wifi_manager_connect) y, cuando retorna con éxito, puede asumir que ya
 * hay una interfaz de red utilizable para abrir sockets.
 *
 * CON QUÉ se comunica: con el subsistema de gestión de red de Zephyr
 * (net_mgmt / net_if) y, a través de él, con el driver Wi-Fi de la ESP32.
 * No toca sockets TCP directamente -- eso es responsabilidad exclusiva de
 * tcp_server.c.
 */

#include <string.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/dhcpv4.h>

#include "wifi_manager.h"
#include "network_config.h"

LOG_MODULE_REGISTER(wifi_manager, LOG_LEVEL_INF);

/* PARÁMETROS: máscaras de bits que indican a qué eventos del stack de red
 * queremos suscribirnos. Se combinan con '|' porque net_mgmt_event_callback
 * acepta un ÚNICO callback "enterado" de varios eventos relacionados; el
 * propio callback distingue cuál ocurrió con un switch() sobre mgmt_event.
 *   - NET_EVENT_WIFI_CONNECT_RESULT:    la asociación Wi-Fi terminó (con
 *     éxito o error) -- se usa para decidir cuándo arrancar el DHCP.
 *   - NET_EVENT_WIFI_DISCONNECT_RESULT: se perdió la conexión Wi-Fi -- solo
 *     se loguea, no hay lógica de reconexión automática en este proyecto
 *     (posible extensión).
 *   - NET_EVENT_IPV4_ADDR_ADD: el cliente DHCP obtuvo una IP -- es la señal
 *     real de "la red ya está lista para abrir sockets".
 *
 * Por qué CALLBACKS y no un bucle que consulte el estado: es el mecanismo
 * asíncrono que expone el stack de red de Zephyr para eventos de gestión
 * (net_mgmt); usarlo evita que este hilo consuma CPU revisando "¿ya hay
 * IP?" en un bucle, y evita también una condición de carrera entre "el
 * evento ya ocurrió" y "el hilo recién empezó a preguntar". */
#define WIFI_MGMT_EVENTS (NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT)
#define IPV4_MGMT_EVENTS (NET_EVENT_IPV4_ADDR_ADD)

/* Estructuras que el subsistema net_mgmt usa internamente para encadenar
 * este callback a su lista de suscriptores. Son "static" (una por tipo de
 * evento) porque deben permanecer vivas mientras la suscripción esté
 * activa -- net_mgmt NO copia su contenido, guarda un puntero. */
static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;

/* Semáforo binario (cuenta máxima 1) usado SOLO para que
 * wifi_manager_connect() pueda bloquearse hasta que el callback de IPv4
 * confirme la IP. No es el mismo semáforo que usa tcp_server.c/rx_processor.c
 * (ese es tcp_server_rx_ready): cada semáforo tiene un único propósito. */
static struct k_sem got_ip_sem;

static void wifi_mgmt_event_handler(struct net_mgmt_event_callback *cb,
				     uint32_t mgmt_event, struct net_if *iface)
{
	switch (mgmt_event) {
	case NET_EVENT_WIFI_CONNECT_RESULT: {
		/* COMUNICACIÓN: cb->info apunta a una struct wifi_status que
		 * el DRIVER Wi-Fi rellenó antes de disparar el evento; es
		 * la forma en que el driver "devuelve" el resultado de la
		 * asociación (éxito/código de error) a quien esté escuchando. */
		const struct wifi_status *status = (const struct wifi_status *)cb->info;

		if (status != NULL && status->status) {
			LOG_ERR("Fallo al conectar a la red Wi-Fi (status=%d)", status->status);
			break;
		}

		LOG_INF("Wi-Fi asociada correctamente, iniciando cliente DHCP...");
		/* POR QUÉ llamar a net_dhcpv4_start() justo aquí: recién
		 * ahora la interfaz tiene enlace (link) activo; pedir una IP
		 * antes de este punto no tendría con quién negociar.
		 * PARÁMETRO iface: la interfaz de red por la que se debe
		 * negociar el DHCP -- la misma que reportó el evento. */
		net_dhcpv4_start(iface);
		break;
	}
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		LOG_WRN("Wi-Fi desconectada");
		break;
	default:
		break;
	}
}

static void ipv4_mgmt_event_handler(struct net_mgmt_event_callback *cb,
				     uint32_t mgmt_event, struct net_if *iface)
{
	ARG_UNUSED(cb);

	if (mgmt_event != NET_EVENT_IPV4_ADDR_ADD) {
		return;
	}

	/* COMUNICACIÓN: lee la primera dirección unicast IPv4 configurada
	 * en la interfaz -- es la IP que el cliente DHCP acaba de instalar.
	 * Se imprime por log porque es el dato que el usuario necesita
	 * copiar a mano en pc_client/main.py (no hay descubrimiento
	 * automático de IP en este proyecto). */
	char buf[NET_IPV4_ADDR_LEN];
	const struct in_addr *addr = &iface->config.ip.ipv4->unicast[0].ipv4.address.in_addr;

	net_addr_ntop(AF_INET, addr, buf, sizeof(buf));
	LOG_INF("Direccion IP obtenida por DHCP: %s  <-- usa esta IP en main.py de la PC", buf);

	/* Libera a wifi_manager_connect(), que está bloqueada esperando
	 * exactamente esta señal (ver más abajo). */
	k_sem_give(&got_ip_sem);
}

int wifi_manager_connect(void)
{
	/* PARÁMETRO: net_if_get_default() devuelve la interfaz de red
	 * "principal" registrada por el driver (en este proyecto, la única:
	 * la interfaz Wi-Fi de la ESP32). No hace falta buscarla por nombre
	 * porque no hay otra interfaz (Ethernet, etc.) compitiendo. */
	struct net_if *iface = net_if_get_default();

	if (iface == NULL) {
		LOG_ERR("No se encontro una interfaz de red por defecto");
		return -ENODEV;
	}

	/* PARÁMETROS de k_sem_init(sem, initial_count, limit): arranca en 0
	 * (nadie ha "avisado" nada todavía) con límite 1 (semáforo BINARIO:
	 * o está señalizado o no lo está, no acumula cuentas). */
	k_sem_init(&got_ip_sem, 0, 1);

	/* PARÁMETROS de net_mgmt_init_event_callback(cb, handler, mask):
	 * asocia la función handler a los eventos indicados por 'mask' DENTRO
	 * de la estructura 'cb'; net_mgmt_add_event_callback() es lo que
	 * realmente registra 'cb' en la lista global de suscriptores del
	 * subsistema net_mgmt -- a partir de esta llamada, el kernel invocará
	 * wifi_mgmt_event_handler() de forma asíncrona cuando ocurra
	 * cualquiera de los eventos en WIFI_MGMT_EVENTS. */
	net_mgmt_init_event_callback(&wifi_cb, wifi_mgmt_event_handler, WIFI_MGMT_EVENTS);
	net_mgmt_add_event_callback(&wifi_cb);

	net_mgmt_init_event_callback(&ipv4_cb, ipv4_mgmt_event_handler, IPV4_MGMT_EVENTS);
	net_mgmt_add_event_callback(&ipv4_cb);

	/* PARÁMETROS de wifi_connect_req_params: es el "formulario" que
	 * espera la API de gestión Wi-Fi de Zephyr para iniciar una conexión.
	 *   - ssid/ssid_length, psk/psk_length: se toman de network_config.h
	 *     (WIFI_SSID/WIFI_PASSWORD) -- strlen() calcula la longitud en
	 *     tiempo de ejecución para no hardcodear un número que se
	 *     desincronice si alguien cambia el string.
	 *   - security = WIFI_SECURITY_TYPE_PSK: WPA2-PSK, el modo más común
	 *     en routers domésticos; usar otro valor aquí es la forma de
	 *     soportar redes abiertas o WPA3 si tu red lo requiere.
	 *   - channel = WIFI_CHANNEL_ANY: no fuerza un canal Wi-Fi específico,
	 *     deja que la ESP32 escanee y elija el canal donde encuentre el
	 *     SSID configurado.
	 *   - band = WIFI_FREQ_BAND_2_4_GHZ: la ESP32 (módulo clásico) solo
	 *     soporta 2.4 GHz; fijarlo evita que intente buscar en 5 GHz
	 *     (banda que no tiene hardware para usar). */
	struct wifi_connect_req_params params = {
		.ssid = (const uint8_t *)WIFI_SSID,
		.ssid_length = strlen(WIFI_SSID),
		.psk = (const uint8_t *)WIFI_PASSWORD,
		.psk_length = strlen(WIFI_PASSWORD),
		.security = WIFI_SECURITY_TYPE_PSK,
		.channel = WIFI_CHANNEL_ANY,
		.band = WIFI_FREQ_BAND_2_4_GHZ,
	};

	LOG_INF("Conectando a la red Wi-Fi \"%s\"...", WIFI_SSID);

	/* PARÁMETROS de net_mgmt(cmd, iface, data, len): 'cmd'
	 * (NET_REQUEST_WIFI_CONNECT) le dice al subsistema QUÉ operación de
	 * gestión ejecutar; 'iface' EN QUÉ interfaz; 'data'/'len' los
	 * parámetros específicos de esa operación (aquí, 'params' de arriba).
	 * Esta llamada es asíncrona: dispara la asociación Wi-Fi y retorna
	 * de inmediato (0 si el pedido fue aceptado, no si ya se conectó);
	 * el resultado real llega más tarde vía NET_EVENT_WIFI_CONNECT_RESULT. */
	int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));

	if (ret) {
		LOG_ERR("net_mgmt(NET_REQUEST_WIFI_CONNECT) fallo: %d", ret);
		return ret;
	}

	/* PARÁMETRO K_SECONDS(30): tiempo máximo a esperar una IP antes de
	 * darse por vencido y devolver error a main() (que a su vez decide
	 * no arrancar las tareas de red). 30 s da margen para escaneo +
	 * asociación + negociación DHCP en una red típica sin bloquear el
	 * arranque indefinidamente si algo salió mal (SSID inexistente,
	 * contraseña incorrecta, etc.). k_sem_take() bloquea este hilo SIN
	 * consumir CPU (no es un busy-wait) hasta que el semáforo se
	 * libere o venza el timeout. */
	if (k_sem_take(&got_ip_sem, K_SECONDS(30)) != 0) {
		LOG_ERR("Tiempo de espera agotado esperando IP por DHCP");
		return -ETIMEDOUT;
	}

	return 0;
}
