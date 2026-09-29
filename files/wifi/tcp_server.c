/*
 * tcp_server.c
 *
 * Este módulo escucha conexiones TCP entrantes y, cuando llegan datos,
 * actúa como el "manejador de interrupción" pedido en el enunciado:
 *
 *   - Zephyr, igual que cualquier RTOS con pila TCP/IP, NO expone a la
 *     aplicación una IRQ de hardware para "el socket tiene datos": la
 *     interrupción real del hardware Wi-Fi la atiende internamente el
 *     driver, varias capas por debajo del API de sockets. Simular eso
 *     aquí sería falso.
 *   - Lo que SÍ se implementa, fielmente, es la disciplina que se le
 *     exige a una ISR: el hilo de este módulo hace el trabajo MÍNIMO
 *     posible (mover los bytes ya disponibles del socket a un buffer
 *     interno) y de inmediato libera un semáforo binario
 *     (tcp_server_rx_ready), delegando TODO el procesamiento pesado
 *     (parseo JSON, verificación de CRC, log detallado, acción local) a
 *     una tarea separada y bloqueada -- rx_processor.c -- tal como una
 *     ISR real delega su trabajo a un hilo en vez de hacerlo ella misma.
 */

#include <string.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/sys/util.h>

#include "tcp_server.h"

LOG_MODULE_REGISTER(tcp_server, LOG_LEVEL_INF);

#define TCP_SERVER_STACK_SIZE 4096
#define TCP_SERVER_PRIORITY   5
#define POLL_TIMEOUT_MS       500
#define RECV_CHUNK_SIZE       128
#define SHARED_BUF_SIZE       256

/* K_MUTEX_DEFINE / K_SEM_DEFINE inicializan estos objetos de forma
 * ESTÁTICA (antes de que main() empiece a correr), a diferencia de
 * k_mutex_init()/k_sem_init() llamados dentro de un hilo en tiempo de
 * ejecución. Esto elimina cualquier condición de carrera entre el orden
 * de arranque de los hilos de la aplicación y el primer uso de estos
 * objetos (p. ej. rx_processor arrancando antes de que este módulo
 * termine de inicializar el semáforo). */
K_MUTEX_DEFINE(client_mutex);
K_MUTEX_DEFINE(shared_buf_mutex);
K_SEM_DEFINE(tcp_server_rx_ready, 0, 1);

static int client_fd = -1;
static char shared_buf[SHARED_BUF_SIZE];
static size_t shared_buf_len;

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

	if (zsock_send(fd, frame, len, 0) < 0) {
		LOG_WRN("Error enviando datos al cliente (errno=%d)", errno);
	}

	return true;
}

size_t tcp_server_drain_rx(char *dst, size_t dst_max)
{
	k_mutex_lock(&shared_buf_mutex, K_FOREVER);

	size_t n = MIN(dst_max, shared_buf_len);

	if (n > 0) {
		memcpy(dst, shared_buf, n);
		memmove(shared_buf, shared_buf + n, shared_buf_len - n);
		shared_buf_len -= n;
	}

	k_mutex_unlock(&shared_buf_mutex);

	return n;
}

/* Equivalente, a nivel de aplicación, del "cuerpo de la ISR": trabajo
 * mínimo (copiar bytes a un buffer compartido) y señalización inmediata.
 * NO parsea JSON, NO valida CRC, NO imprime detalles del contenido. */
static void notify_data_available(const char *data, size_t len)
{
	k_mutex_lock(&shared_buf_mutex, K_FOREVER);

	size_t space = sizeof(shared_buf) - shared_buf_len;
	size_t to_copy = MIN(len, space);

	if (to_copy < len) {
		LOG_WRN("Buffer compartido de RX lleno: se descartan %d bytes "
			"(la tarea de procesamiento no está drenando a tiempo)",
			(int)(len - to_copy));
	}

	memcpy(shared_buf + shared_buf_len, data, to_copy);
	shared_buf_len += to_copy;

	k_mutex_unlock(&shared_buf_mutex);

	/* "Interrupción" de aplicación: despierta a rx_processor_thread_fn,
	 * que está bloqueada en k_sem_take(&tcp_server_rx_ready, K_FOREVER). */
	k_sem_give(&tcp_server_rx_ready);
}

static void tcp_server_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

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

		bool client_connected = true;

		/* Bucle "notificador": usa poll() con timeout para detectar
		 * datos de forma asincrona (sin bloquear indefinidamente en
		 * un unico recv()), y en cuanto llegan, hace el traspaso
		 * minimo descrito arriba. */
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
				char chunk[RECV_CHUNK_SIZE];
				ssize_t n = zsock_recv(fd, chunk, sizeof(chunk), 0);

				if (n <= 0) {
					LOG_INF("Cliente desconectado");
					client_connected = false;
				} else {
					notify_data_available(chunk, (size_t)n);
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

void tcp_server_start(void)
{
	k_thread_start(tcp_server_tid);
}
