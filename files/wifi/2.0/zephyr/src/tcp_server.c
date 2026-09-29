/*
 * tcp_server.c
 * =============
 * POR QUÉ este módulo existe: es el único lugar del proyecto que toca la
 * API de sockets BSD de Zephyr. Aísla "cómo se acepta una conexión TCP y
 * se detectan datos entrantes" de "qué significan esos datos" (eso vive
 * en rx_processor.c + json_protocol.c).
 *
 * Este módulo actúa, deliberadamente, como el "manejador de interrupción"
 * pedido en el enunciado:
 *   - Zephyr, igual que cualquier RTOS con pila TCP/IP, NO expone a la
 *     aplicación una IRQ de hardware para "el socket tiene datos": esa
 *     interrupción real la atiende internamente el driver Wi-Fi, varias
 *     capas por debajo del API de sockets. Simular eso aquí sería falso.
 *   - Lo que SÍ se implementa, fielmente, es la disciplina que se le
 *     exige a una ISR: el hilo de este módulo hace el trabajo MÍNIMO
 *     posible (mover los bytes ya disponibles del socket a un buffer
 *     interno) y de inmediato libera un semáforo binario
 *     (tcp_server_rx_ready), delegando TODO el procesamiento pesado
 *     (decodificación JSON, verificación de CRC, log detallado, acción
 *     local) a una tarea separada y bloqueada — rx_processor.c — tal
 *     como una ISR real delega su trabajo a un hilo en vez de hacerlo
 *     ella misma.
 */

#include <string.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/sys/util.h>

#include "tcp_server.h"
#include "network_config.h"

LOG_MODULE_REGISTER(tcp_server, LOG_LEVEL_INF);

/* PARÁMETROS de tamaño/prioridad de hilo (usados en K_THREAD_DEFINE al
 * final del archivo):
 *   - TCP_SERVER_STACK_SIZE = 4096 B: cubre las estructuras locales de
 *     zsock_accept()/zsock_poll() (sockaddr_in, pollfd) más el margen que
 *     pide el propio stack de red de Zephyr para sus llamadas internas;
 *     4 KiB es el tamaño habitual en los ejemplos de red de Zephyr para
 *     hilos que solo manejan sockets (sin JSON pesado en este hilo).
 *   - TCP_SERVER_PRIORITY = 5: prioridad cooperativa/preemptiva media
 *     (Zephyr: números MÁS BAJOS son MÁS prioritarios). Se usa la misma
 *     prioridad que rx_processor.c y telemetry_tx.c porque ninguna de las
 *     tres tareas es más urgente que las otras dos: el scheduler las
 *     alterna por turnos (round-robin) cuando las tres están listas. */
#define TCP_SERVER_STACK_SIZE 4096
#define TCP_SERVER_PRIORITY   5

/* PARÁMETRO POLL_TIMEOUT_MS = 500: cada cuánto zsock_poll() "despierta"
 * al hilo aunque NO haya datos, solo para volver a evaluar el bucle
 * (p. ej. permitiría agregar más adelante una condición de salida). Un
 * valor demasiado bajo desperdicia ciclos de CPU despertando sin motivo;
 * uno demasiado alto haría más lenta la reacción a un cliente que se
 * desconecta silenciosamente. 500 ms es un punto medio típico para este
 * tipo de bucle de atención a un único cliente. */
#define POLL_TIMEOUT_MS       500

/* PARÁMETRO RECV_CHUNK_SIZE = 128: tamaño del buffer de pila donde se
 * copia cada recv() individual ANTES de pasarlo al buffer compartido.
 * 128 B es más que suficiente para una trama de este protocolo (~90-120
 * B), pero pequeño para no presionar el stack del hilo. */
#define RECV_CHUNK_SIZE       128

/* PARÁMETRO SHARED_BUF_SIZE = 256: tamaño del buffer compartido con
 * rx_processor.c (ver notify_data_available/tcp_server_drain_rx). Cubre
 * varias tramas en cola por si la tarea de procesamiento tarda un poco
 * en drenar, sin reservar memoria innecesaria en un microcontrolador. */
#define SHARED_BUF_SIZE       256

/* K_MUTEX_DEFINE / K_SEM_DEFINE inicializan estos objetos de forma
 * ESTÁTICA (antes de que main() empiece a correr), a diferencia de
 * k_mutex_init()/k_sem_init() llamados dentro de un hilo en tiempo de
 * ejecución. Esto elimina cualquier condición de carrera entre el orden
 * de arranque de los hilos de la aplicación y el primer uso de estos
 * objetos (p. ej. rx_processor arrancando antes de que este módulo
 * termine de inicializar el semáforo).
 *   - client_mutex:     protege la variable 'client_fd' (leída por
 *     tcp_server_send(), escrita por el hilo de este archivo).
 *   - shared_buf_mutex: protege 'shared_buf'/'shared_buf_len' (escritos
 *     aquí, leídos/consumidos por tcp_server_drain_rx() desde el hilo de
 *     rx_processor.c).
 *   - tcp_server_rx_ready: el semáforo binario "tipo ISR" (ver tcp_server.h). */
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
		return false; /* nadie conectado todavia: no es un error, solo "no hay a quien enviar" */
	}

	/* COMUNICACIÓN: envía 'frame' (ya codificado en JSON + '\n' por
	 * json_protocol_build_telemetry) al socket TCP conectado 'fd'.
	 * PARÁMETRO flags=0: sin banderas especiales (no se necesita
	 * MSG_DONTWAIT ni similares; el socket ya es el de un cliente
	 * aceptado y send() en un socket TCP conectado no debería bloquear
	 * para tramas tan pequeñas). */
	if (zsock_send(fd, frame, len, 0) < 0) {
		LOG_WRN("Error enviando datos al cliente (errno=%d)", errno);
	}

	return true;
}

size_t tcp_server_drain_rx(char *dst, size_t dst_max)
{
	k_mutex_lock(&shared_buf_mutex, K_FOREVER);

	/* PARÁMETRO: MIN(dst_max, shared_buf_len) -- nunca copia más de lo
	 * que el llamador puede recibir (dst_max) NI más de lo que
	 * realmente hay disponible (shared_buf_len). */
	size_t n = MIN(dst_max, shared_buf_len);

	if (n > 0) {
		memcpy(dst, shared_buf, n);
		/* Desplaza el resto (lo no copiado) al inicio del buffer:
		 * mantiene shared_buf como una cola FIFO simple. */
		memmove(shared_buf, shared_buf + n, shared_buf_len - n);
		shared_buf_len -= n;
	}

	k_mutex_unlock(&shared_buf_mutex);

	return n;
}

/* Equivalente, a nivel de aplicación, del "cuerpo de la ISR": trabajo
 * mínimo (copiar bytes a un buffer compartido) y señalización inmediata.
 * NO decodifica JSON, NO valida CRC, NO imprime el contenido recibido. */
static void notify_data_available(const char *data, size_t len)
{
	k_mutex_lock(&shared_buf_mutex, K_FOREVER);

	size_t space = sizeof(shared_buf) - shared_buf_len;
	size_t to_copy = MIN(len, space);

	if (to_copy < len) {
		/* Backpressure simple: si rx_processor.c no está drenando a
		 * tiempo (buffer lleno), se descarta el exceso y se avisa
		 * por log, en vez de bloquear indefinidamente al notificador
		 * (que dejaría de atender al socket). */
		LOG_WRN("Buffer compartido de RX lleno: se descartan %d bytes "
			"(la tarea de procesamiento no esta drenando a tiempo)",
			(int)(len - to_copy));
	}

	memcpy(shared_buf + shared_buf_len, data, to_copy);
	shared_buf_len += to_copy;

	k_mutex_unlock(&shared_buf_mutex);

	/* "Interrupción" de aplicación: despierta a rx_processor_thread_fn,
	 * que está bloqueada en k_sem_take(&tcp_server_rx_ready, K_FOREVER).
	 * PARÁMETRO: k_sem_give() no necesita argumentos además del
	 * semáforo -- un semáforo BINARIO no "cuenta" cuántas veces se
	 * llamó, solo pasa a estado "señalizado" (si ya lo estaba, esta
	 * llamada es un no-op). Por eso rx_processor.c drena en un bucle
	 * hasta vaciar el buffer: una sola señal puede representar más de
	 * una ráfaga de datos. */
	k_sem_give(&tcp_server_rx_ready);
}

static void tcp_server_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	/* PARÁMETROS de zsock_socket(family, type, protocol):
	 *   - AF_INET: direcciones IPv4 (coherente con CONFIG_NET_IPV4=y y
	 *     CONFIG_NET_IPV6=n en prj.conf).
	 *   - SOCK_STREAM: socket de FLUJO orientado a conexión -- lo que
	 *     pide TCP (a diferencia de SOCK_DGRAM para UDP).
	 *   - IPPROTO_TCP: protocolo de transporte explícito. */
	int listen_fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (listen_fd < 0) {
		LOG_ERR("No se pudo crear el socket de escucha (errno=%d)", errno);
		return;
	}

	/* PARÁMETRO SO_REUSEADDR=1: permite volver a hacer bind() al mismo
	 * puerto inmediatamente después de un reinicio/reflasheo, aunque el
	 * sistema operativo todavía tenga el puerto anterior en estado
	 * TIME_WAIT. Sin esto, un reinicio rápido durante pruebas podría
	 * fallar el bind() con "address already in use". */
	int reuse = 1;

	zsock_setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

	/* PARÁMETROS de la dirección de bind:
	 *   - sin_family = AF_INET: coherente con el socket creado arriba.
	 *   - sin_port = htons(TCP_SERVER_PORT): TCP_SERVER_PORT viene de
	 *     network_config.h (editable sin tocar este archivo). htons()
	 *     convierte de "orden de host" a "orden de red" (big-endian),
	 *     requisito del protocolo IP independientemente de la
	 *     arquitectura del microcontrolador.
	 *   - sin_addr.s_addr = htonl(INADDR_ANY): escucha en TODAS las
	 *     interfaces/direcciones locales disponibles (en este proyecto,
	 *     efectivamente la única IP que tiene la ESP32 tras el DHCP). */
	struct sockaddr_in bind_addr = {
		.sin_family = AF_INET,
		.sin_port = htons(TCP_SERVER_PORT),
		.sin_addr.s_addr = htonl(INADDR_ANY),
	};

	if (zsock_bind(listen_fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
		LOG_ERR("No se pudo asociar (bind) el puerto %d (errno=%d)",
			TCP_SERVER_PORT, errno);
		zsock_close(listen_fd);
		return;
	}

	/* PARÁMETRO backlog=1 de zsock_listen(): cuántas conexiones
	 * entrantes puede "hacer cola" el sistema operativo antes de que la
	 * aplicación llame a accept(). Este proyecto atiende UN cliente
	 * (la app de PC) a la vez, así que 1 es suficiente y deja explícita
	 * esa decisión de diseño. */
	if (zsock_listen(listen_fd, 1) < 0) {
		LOG_ERR("No se pudo poner el socket en escucha (errno=%d)", errno);
		zsock_close(listen_fd);
		return;
	}

	LOG_INF("Servidor TCP escuchando en el puerto %d", TCP_SERVER_PORT);

	for (;;) {
		LOG_INF("Esperando conexion de un cliente (la app de PC)...");

		struct sockaddr_in client_addr;
		socklen_t addr_len = sizeof(client_addr);

		/* COMUNICACIÓN: bloquea este hilo hasta que la app de PC
		 * inicie una conexión TCP hacia TCP_SERVER_PORT. Es
		 * "bloqueante" a propósito: este hilo no tiene nada mejor
		 * que hacer mientras no hay ningún cliente. */
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
			/* PARÁMETROS de struct zsock_pollfd:
			 *   - fd: el socket del cliente ya aceptado.
			 *   - events = ZSOCK_POLLIN: "avísame cuando haya
			 *     datos para leer" (no nos interesa POLLOUT
			 *     porque el envío lo hace telemetry_tx.c con
			 *     send() directo, sin necesitar poll previo). */
			struct zsock_pollfd pfd = {
				.fd = fd,
				.events = ZSOCK_POLLIN,
			};

			/* PARÁMETROS de zsock_poll(fds, nfds, timeout):
			 *   - nfds=1: un único socket a vigilar.
			 *   - timeout=POLL_TIMEOUT_MS: no espera para siempre
			 *     (a diferencia de un recv() bloqueante puro), así
			 *     el bucle puede revisar periódicamente el estado
			 *     de client_connected sin quedar atascado si el
			 *     cliente nunca más envía nada. */
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
				/* PARÁMETRO flags=0: lectura estándar, sin
				 * MSG_PEEK ni MSG_WAITALL -- se procesa lo
				 * que haya disponible en este momento, sin
				 * esperar a completar 'chunk'. */
				ssize_t n = zsock_recv(fd, chunk, sizeof(chunk), 0);

				if (n <= 0) {
					/* recv() devuelve 0 cuando el otro
					 * lado cerró la conexión de forma
					 * ordenada (FIN), y <0 ante un error
					 * real -- ambos casos, para este
					 * proyecto, significan "ya no hay
					 * cliente". */
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

/* PARÁMETROS de K_THREAD_DEFINE(nombre, stack_size, entry, p1, p2, p3,
 * priority, options, delay): crea y describe el hilo de forma ESTÁTICA
 * (en tiempo de compilación/enlazado), sin necesitar k_thread_create() en
 * tiempo de ejecución.
 *   - p1/p2/p3 = NULL: esta función de hilo no necesita parámetros de
 *     entrada (toda su configuración viene de network_config.h y de los
 *     #define de este archivo).
 *   - options=0: sin banderas especiales (p. ej. no hace falta
 *     K_FP_REGS, este hilo no usa punto flotante).
 *   - delay=K_FOREVER: el hilo se crea SUSPENDIDO -- no arranca solo. Es
 *     intencional: intentar bind()/listen() antes de que wifi_manager
 *     confirme que hay IP fallaría. tcp_server_start() lo arranca
 *     explícitamente, en el momento correcto, desde main(). */
K_THREAD_DEFINE(tcp_server_tid, TCP_SERVER_STACK_SIZE, tcp_server_thread,
		 NULL, NULL, NULL, TCP_SERVER_PRIORITY, 0, K_FOREVER);

void tcp_server_start(void)
{
	/* PARÁMETRO: tcp_server_tid es el identificador de hilo que generó
	 * K_THREAD_DEFINE arriba; k_thread_start() lo mueve de "suspendido"
	 * a "listo para ejecutar", y el scheduler de Zephyr lo despachará
	 * según su prioridad tan pronto como haya un núcleo disponible. */
	k_thread_start(tcp_server_tid);
}
