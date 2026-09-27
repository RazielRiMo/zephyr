#include <string.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_ip.h>

#include "tcp_server.h"
#include "json_protocol.h"

LOG_MODULE_REGISTER(tcp_server, LOG_LEVEL_INF);

#define TCP_SERVER_STACK_SIZE 4096
#define TCP_SERVER_PRIORITY   5
#define RX_BUF_SIZE           JSON_FRAME_MAX_LEN
#define POLL_TIMEOUT_MS       500

static struct k_mutex client_mutex;
static int client_fd = -1;
static uint32_t crc_error_count;
static tcp_server_on_command_t command_callback;

static void set_client_fd(int fd)
{
	k_mutex_lock(&client_mutex, K_FOREVER);
	client_fd = fd;
	k_mutex_unlock(&client_mutex);
}

static int get_client_fd(void)
{
	k_mutex_lock(&client_mutex, K_FOREVER);
	int fd = client_fd;
	k_mutex_unlock(&client_mutex);
	return fd;
}

bool tcp_server_send(const char *frame, size_t len)
{
	int fd = get_client_fd();

	if (fd < 0) {
		return false;
	}

	ssize_t sent = zsock_send(fd, frame, len, 0);

	if (sent < 0) {
		LOG_WRN("Error enviando datos al cliente (errno=%d)", errno);
	}

	return true;
}

uint32_t tcp_server_get_crc_error_count(void)
{
	return crc_error_count;
}

/* El buffer de recepción puede contener 0, 1 o varias tramas separadas por
 * '\n' (framing por delimitador: TCP es un flujo de bytes sin límites de
 * mensaje propios, así que hace falta un criterio para saber dónde termina
 * cada trama JSON). Procesa todas las tramas completas y conserva el resto
 * incompleto al inicio del buffer para la próxima recepción. */
static void process_rx_buffer(char *buf, size_t *len)
{
	char *start = buf;
	char *buf_end = buf + *len;

	for (;;) {
		char *newline = memchr(start, '\n', (size_t)(buf_end - start));

		if (newline == NULL) {
			break;
		}

		size_t line_len = (size_t)(newline - start);
		struct json_frame_result result;

		json_validate_frame(start, line_len, &result);

		if (!result.format_ok) {
			LOG_WRN("Trama descartada: no se encontro el campo \"crc\"");
		} else if (!result.crc_ok) {
			crc_error_count++;
			LOG_WRN("=== ERROR DE CRC EN TRAMA RECIBIDA DESDE LA PC ===");
			LOG_WRN("  CRC recibido  : 0x%04X", result.crc_received);
			LOG_WRN("  CRC calculado : 0x%04X", result.crc_computed);
			LOG_WRN("  Trama (%d bytes) descartada por integridad", (int)line_len);
			LOG_WRN("  Contador total de errores de CRC: %u", crc_error_count);
		} else {
			LOG_INF("Trama valida recibida (value=%d, has_value=%d)",
				result.value, result.has_value);
			if (command_callback != NULL) {
				command_callback(result.value, result.has_value);
			}
		}

		start = newline + 1;
	}

	size_t remaining = (size_t)(buf_end - start);

	memmove(buf, start, remaining);
	*len = remaining;
}

static void tcp_server_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	k_mutex_init(&client_mutex);

	int listen_fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (listen_fd < 0) {
		LOG_ERR("No se pudo crear el socket de escucha (errno=%d)", errno);
		return;
	}

	int reuse = 1;

	zsock_setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

	struct sockaddr_in bind_addr = {
		.sin_family = AF_INET,
		.sin_port = htons(CONFIG_APP_TCP_PORT),
		.sin_addr.s_addr = htonl(INADDR_ANY),
	};

	if (zsock_bind(listen_fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
		LOG_ERR("No se pudo asociar (bind) el puerto %d (errno=%d)",
			CONFIG_APP_TCP_PORT, errno);
		zsock_close(listen_fd);
		return;
	}

	if (zsock_listen(listen_fd, 1) < 0) {
		LOG_ERR("No se pudo poner el socket en escucha (errno=%d)", errno);
		zsock_close(listen_fd);
		return;
	}

	LOG_INF("Servidor TCP escuchando en el puerto %d", CONFIG_APP_TCP_PORT);

	char rx_buf[RX_BUF_SIZE];

	for (;;) {
		LOG_INF("Esperando conexion de un cliente (la app de PC)...");

		struct sockaddr_in client_addr;
		socklen_t addr_len = sizeof(client_addr);

		int fd = zsock_accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);

		if (fd < 0) {
			LOG_ERR("accept() fallo (errno=%d)", errno);
			k_sleep(K_MSEC(1000));
			continue;
		}

		char addr_str[NET_IPV4_ADDR_LEN];

		net_addr_ntop(AF_INET, &client_addr.sin_addr, addr_str, sizeof(addr_str));
		LOG_INF("Cliente conectado desde %s:%d", addr_str, ntohs(client_addr.sin_port));

		set_client_fd(fd);

		size_t rx_len = 0;
		bool client_connected = true;

		/* Bucle de atencion al cliente: usa poll() con timeout para
		 * comprobar la llegada de datos de forma asincrona en vez de
		 * bloquear este hilo indefinidamente en un unico recv(). */
		while (client_connected) {
			struct zsock_pollfd pfd = {
				.fd = fd,
				.events = ZSOCK_POLLIN,
			};

			int poll_ret = zsock_poll(&pfd, 1, POLL_TIMEOUT_MS);

			if (poll_ret < 0) {
				LOG_ERR("poll() fallo (errno=%d)", errno);
				break;
			}

			if (poll_ret == 0) {
				continue; /* timeout sin datos: vuelve a intentar */
			}

			if (pfd.revents & ZSOCK_POLLIN) {
				ssize_t n = zsock_recv(fd, rx_buf + rx_len,
							sizeof(rx_buf) - rx_len - 1, 0);

				if (n <= 0) {
					LOG_INF("Cliente desconectado");
					client_connected = false;
				} else {
					rx_len += (size_t)n;
					rx_buf[rx_len] = '\0';
					process_rx_buffer(rx_buf, &rx_len);
				}
			}

			if (pfd.revents & (ZSOCK_POLLHUP | ZSOCK_POLLERR)) {
				LOG_INF("Conexion cerrada por el cliente");
				client_connected = false;
			}
		}

		set_client_fd(-1);
		zsock_close(fd);
	}
}

/* Se crea en estado suspendido (K_FOREVER como "delay" inicial) para que
 * el hilo NO intente usar la pila de red antes de que main() confirme que
 * la Wi-Fi ya tiene IP. tcp_server_start() lo arranca explícitamente. */
K_THREAD_DEFINE(tcp_server_tid, TCP_SERVER_STACK_SIZE, tcp_server_thread,
		 NULL, NULL, NULL, TCP_SERVER_PRIORITY, 0, K_FOREVER);

void tcp_server_start(tcp_server_on_command_t on_command)
{
	command_callback = on_command;
	k_thread_start(tcp_server_tid);
}
