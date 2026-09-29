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

LOG_MODULE_REGISTER(wifi_manager, LOG_LEVEL_INF);

/* Eventos del stack de red que nos interesan. Se atienden con callbacks
 * asíncronos (net_mgmt), nunca sondeando el estado en un bucle. */
#define WIFI_MGMT_EVENTS (NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT)
#define IPV4_MGMT_EVENTS (NET_EVENT_IPV4_ADDR_ADD)

static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;
static struct k_sem got_ip_sem;

static void wifi_mgmt_event_handler(struct net_mgmt_event_callback *cb,
				     uint32_t mgmt_event, struct net_if *iface)
{
	switch (mgmt_event) {
	case NET_EVENT_WIFI_CONNECT_RESULT: {
		const struct wifi_status *status = (const struct wifi_status *)cb->info;

		if (status != NULL && status->status) {
			LOG_ERR("Fallo al conectar a la red Wi-Fi (status=%d)", status->status);
			break;
		}

		LOG_INF("Wi-Fi asociada correctamente, iniciando cliente DHCP...");
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

	char buf[NET_IPV4_ADDR_LEN];
	const struct in_addr *addr = &iface->config.ip.ipv4->unicast[0].ipv4.address.in_addr;

	net_addr_ntop(AF_INET, addr, buf, sizeof(buf));
	LOG_INF("Direccion IP obtenida por DHCP: %s  <-- usa esta IP en main.py de la PC", buf);

	k_sem_give(&got_ip_sem);
}

int wifi_manager_connect(void)
{
	struct net_if *iface = net_if_get_default();

	if (iface == NULL) {
		LOG_ERR("No se encontro una interfaz de red por defecto");
		return -ENODEV;
	}

	k_sem_init(&got_ip_sem, 0, 1);

	net_mgmt_init_event_callback(&wifi_cb, wifi_mgmt_event_handler, WIFI_MGMT_EVENTS);
	net_mgmt_add_event_callback(&wifi_cb);

	net_mgmt_init_event_callback(&ipv4_cb, ipv4_mgmt_event_handler, IPV4_MGMT_EVENTS);
	net_mgmt_add_event_callback(&ipv4_cb);

	struct wifi_connect_req_params params = {
		.ssid = (const uint8_t *)CONFIG_APP_WIFI_SSID,
		.ssid_length = strlen(CONFIG_APP_WIFI_SSID),
		.psk = (const uint8_t *)CONFIG_APP_WIFI_PSK,
		.psk_length = strlen(CONFIG_APP_WIFI_PSK),
		.security = WIFI_SECURITY_TYPE_PSK,
		.channel = WIFI_CHANNEL_ANY,
		.band = WIFI_FREQ_BAND_2_4_GHZ,
	};

	LOG_INF("Conectando a la red Wi-Fi \"%s\"...", CONFIG_APP_WIFI_SSID);

	int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));

	if (ret) {
		LOG_ERR("net_mgmt(NET_REQUEST_WIFI_CONNECT) fallo: %d", ret);
		return ret;
	}

	/* Bloquea SIN ocupar CPU (no es un busy-wait) hasta que el callback
	 * de IPv4 confirme que ya hay una direccion asignada por DHCP. */
	if (k_sem_take(&got_ip_sem, K_SECONDS(30)) != 0) {
		LOG_ERR("Tiempo de espera agotado esperando IP por DHCP");
		return -ETIMEDOUT;
	}

	return 0;
}
