/*
 * Proyecto: comunicación bidireccional Wi-Fi/TCP + JSON + CRC
 * Plataforma: ESP32 + Zephyr RTOS 4.4.x
 *
 * Arquitectura:
 *   1) main/network manager : Wi-Fi, DHCP y reconexión TCP.
 *   2) tx_thread             : genera telemetría periódica y la envía.
 *   3) rx_thread             : recibe el stream TCP y separa tramas por '\n'.
 *   4) rx_processing_thread  : espera un semáforo binario y procesa JSON.
 *
 * Protocolo:
 *   Cada mensaje es un JSON terminado en '\n'.
 *   El campo "crc" NO participa en el cálculo.
 *   El CRC se calcula sobre los bytes ASCII/UTF-8 exactos del JSON sin crc.
 *
 * Ejemplo de trama transmitida:
 *   {"type":"telemetry","seq":1,"value":21,"crc":51770}\n
 * donde el CRC se obtiene de:
 *   {"type":"telemetry","seq":1,"value":21}
 *
 * IMPORTANTE SOBRE ISR:
 * BSD sockets no exponen una ISR de aplicación que se ejecute cuando llega
 * un byte TCP. La pila de red procesa RX en contexto de red/hilo y recv()
 * se bloquea/despierta en el hilo de recepción. Por eso la implementación
 * usa un "RX event hook" inmediatamente después de completar una trama para
 * liberar el semáforo binario. Es equivalente a la sincronización buscada,
 * pero NO debe llamarse una ISR real. Forzar una ISR GPIO para representar
 * la llegada TCP sería arquitectónicamente incorrecto.
 */

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/data/json.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/wifi_mgmt.h>

#include "config.h"

LOG_MODULE_REGISTER(wifi_tcp_json, LOG_LEVEL_INF);

/* -------------------------------------------------------------------------- */
/* Constantes / objetos RTOS                                                  */
/* -------------------------------------------------------------------------- */

#define CONN_EVENT_CONNECTED BIT(0)

K_EVENT_DEFINE(conn_event);

/* Semáforo binario solicitado: RX event hook -> processing task. */
K_SEM_DEFINE(rx_sem, 0, 1);

/* Protege el socket compartido por TX/RX y el cierre de conexión. */
K_MUTEX_DEFINE(sock_mutex);

/* Cola de tramas completas. El semáforo sólo notifica que hay datos. */
struct rx_frame {
	uint16_t len;
	char data[MAX_FRAME_SIZE];
};

K_MSGQ_DEFINE(rx_msgq,
	      sizeof(struct rx_frame),
	      RX_QUEUE_LENGTH,
	      4);

/* Threads. */
static K_THREAD_STACK_DEFINE(tx_stack, 4096);
static K_THREAD_STACK_DEFINE(rx_stack, 4096);
static K_THREAD_STACK_DEFINE(proc_stack, 4096);

static struct k_thread tx_thread_data;
static struct k_thread rx_thread_data;
static struct k_thread proc_thread_data;

/* Socket TCP compartido. -1 significa "sin conexión". */
static int tcp_sock = -1;

/* Estado Wi-Fi indicado por NET_EVENT_WIFI_CONNECT_RESULT/DISCONNECT_RESULT. */
static volatile bool wifi_connected;
static struct net_mgmt_event_callback wifi_cb;

/* Contador local requerido de errores CRC. */
static uint32_t crc_error_count;

/* -------------------------------------------------------------------------- */
/* CRC-16-CCITT-FALSE                                                         */
/* Polinomio 0x1021, init 0xFFFF, sin reflexión, xorout 0x0000.              */
/* -------------------------------------------------------------------------- */

static uint16_t crc16_ccitt_false(const uint8_t *data, size_t len)
{
	uint16_t crc = 0xFFFFU;

	for (size_t i = 0; i < len; ++i) {
		crc ^= (uint16_t)data[i] << 8;

		for (int bit = 0; bit < 8; ++bit) {
			if (crc & 0x8000U) {
				crc = (uint16_t)((crc << 1) ^ 0x1021U);
			} else {
				crc <<= 1;
			}
		}
	}

	return crc;
}

/* -------------------------------------------------------------------------- */
/* Utilidades TCP                                                             */
/* -------------------------------------------------------------------------- */

static void connection_clear(void)
{
	k_mutex_lock(&sock_mutex, K_FOREVER);

	if (tcp_sock >= 0) {
		LOG_INF("TCP: cerrando socket %d", tcp_sock);
		(void)zsock_close(tcp_sock);
		tcp_sock = -1;
	}

	k_event_clear(&conn_event, CONN_EVENT_CONNECTED);
	k_mutex_unlock(&sock_mutex);
}

static bool connection_get_socket(int *sock_out)
{
	bool ok = false;

	k_mutex_lock(&sock_mutex, K_FOREVER);
	if (tcp_sock >= 0) {
		*sock_out = tcp_sock;
		ok = true;
	}
	k_mutex_unlock(&sock_mutex);

	return ok;
}

static int send_all(const char *data, size_t len)
{
	int sock;
	size_t sent = 0U;

	/* Evitamos que TX y el ACK de procesamiento escriban simultáneamente. */
	k_mutex_lock(&sock_mutex, K_FOREVER);

	if (tcp_sock < 0) {
		k_mutex_unlock(&sock_mutex);
		return -ENOTCONN;
	}

	sock = tcp_sock;

	while (sent < len) {
		int ret = zsock_send(sock, data + sent, len - sent, 0);

		if (ret < 0) {
			int err = -errno;
			LOG_ERR("TCP send() fallo: errno=%d", errno);
			k_mutex_unlock(&sock_mutex);
			return err;
		}

		if (ret == 0) {
			k_mutex_unlock(&sock_mutex);
			return -EPIPE;
		}

		sent += (size_t)ret;
	}

	k_mutex_unlock(&sock_mutex);
	return 0;
}

/* -------------------------------------------------------------------------- */
/* JSON: estructuras y descriptores                                           */
/* -------------------------------------------------------------------------- */

struct telemetry_json {
	const char *type;
	int32_t seq;
	int32_t value;
	uint32_t crc_errors;
};

static const struct json_obj_descr telemetry_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct telemetry_json, type, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct telemetry_json, seq, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct telemetry_json, value, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct telemetry_json, crc_errors, JSON_TOK_UINT),
};

struct command_json {
	const char *type;
	double value;
};

static const struct json_obj_descr command_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct command_json, type, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct command_json, value, JSON_TOK_DOUBLE_FP),
};

struct ack_json {
	const char *type;
	int32_t status;
	double value;
	uint32_t crc_errors;
};

static const struct json_obj_descr ack_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct ack_json, type, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct ack_json, status, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct ack_json, value, JSON_TOK_DOUBLE_FP),
	JSON_OBJ_DESCR_PRIM(struct ack_json, crc_errors, JSON_TOK_UINT),
};

/*
 * Envuelve un body JSON que termina en '}' y añade ,"crc":NNNN} + '\n'.
 * body debe ser JSON válido y no debe contener ya el campo crc.
 */
static int frame_with_crc(const char *body, size_t body_len,
			  char *frame, size_t frame_size)
{
	uint16_t crc;
	int n;

	if (body_len < 2U || body[0] != '{' || body[body_len - 1U] != '}') {
		return -EINVAL;
	}

	crc = crc16_ccitt_false((const uint8_t *)body, body_len);

	n = snprintf(frame, frame_size, "%.*s,\"crc\":%u}\n",
		     (int)(body_len - 1U), body, crc);

	if (n < 0 || (size_t)n >= frame_size) {
		return -ENOSPC;
	}

	return n;
}

/*
 * Extrae el body sin crc y el CRC recibido.
 * Entrada: una sola línea sin '\n' final obligatorio.
 */
static int parse_crc_frame(const char *frame, size_t frame_len,
			   char *body, size_t body_size,
			   uint16_t *received_crc)
{
	char crc_text[8];
	const char *marker = ",\"crc\":";
	const char *crc_pos;
	const char *end;
	size_t body_without_brace_len;
	size_t crc_digits;
	char *endptr;
	unsigned long crc_value;

	if (frame_len == 0U || body_size < 3U) {
		return -EINVAL;
	}

	/* Aceptamos CRLF por robustez. */
	while (frame_len > 0U &&
	       (frame[frame_len - 1U] == '\n' || frame[frame_len - 1U] == '\r')) {
		frame_len--;
	}

	if (frame_len < strlen(marker) + 3U || frame[0] != '{' ||
	    frame[frame_len - 1U] != '}') {
		return -EINVAL;
	}

	crc_pos = strstr(frame, marker);
	if (crc_pos == NULL) {
		return -EINVAL;
	}

	body_without_brace_len = (size_t)(crc_pos - frame);
	if (body_without_brace_len + 2U > body_size) {
		return -ENOSPC;
	}

	/* Reconstruimos exactamente el JSON que originó el CRC. */
	memcpy(body, frame, body_without_brace_len);
	body[body_without_brace_len] = '}';
	body[body_without_brace_len + 1U] = '\0';

	end = frame + frame_len - 1U;
	crc_digits = (size_t)(end - (crc_pos + strlen(marker)));
	if (crc_digits == 0U || crc_digits >= sizeof(crc_text)) {
		return -EINVAL;
	}

	memcpy(crc_text, crc_pos + strlen(marker), crc_digits);
	crc_text[crc_digits] = '\0';

	for (size_t i = 0U; i < crc_digits; ++i) {
		if (crc_text[i] < '0' || crc_text[i] > '9') {
			return -EINVAL;
		}
	}

	endptr = NULL;
	crc_value = strtoul(crc_text, &endptr, 10);
	if (endptr == NULL || *endptr != '\0' || crc_value > UINT16_MAX) {
		return -EINVAL;
	}

	*received_crc = (uint16_t)crc_value;
	return 0;
}

/* -------------------------------------------------------------------------- */
/* Wi-Fi                                                                      */
/* -------------------------------------------------------------------------- */

static void wifi_mgmt_handler(struct net_mgmt_event_callback *cb,
			      uint64_t event,
			      struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	if (event == NET_EVENT_WIFI_CONNECT_RESULT) {
		/*
		 * En Zephyr 4.4.x el primer campo del dato del evento es el status.
		 * No dependemos del tamaño completo de la estructura para mantener
		 * compatibilidad con cambios posteriores.
		 */
		int status = -1;

		if (cb->info != NULL) {
			memcpy(&status, cb->info, sizeof(status));
		}

		if (status == 0) {
			wifi_connected = true;
			LOG_INF("Wi-Fi conectado");
		} else {
			wifi_connected = false;
			LOG_ERR("Wi-Fi rechazo la conexión: status=%d", status);
		}
	} else if (event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
		wifi_connected = false;
		LOG_WRN("Wi-Fi desconectado");
	}
}

static int wifi_connect(void)
{
	struct net_if *iface = net_if_get_wifi_sta();
	struct wifi_connect_req_params params = {0};
	int ret;
	int retries = 30;

	if (iface == NULL) {
		LOG_ERR("No se encontró interfaz Wi-Fi STA");
		return -ENODEV;
	}

	params.ssid = (const uint8_t *)WIFI_SSID;
	params.ssid_length = strlen(WIFI_SSID);
	params.psk = (const uint8_t *)WIFI_PASSWORD;
	params.psk_length = strlen(WIFI_PASSWORD);
	params.security = WIFI_SECURITY;
	params.channel = 0U;

	wifi_connected = false;

	LOG_INF("Conectando a SSID '%s'...", WIFI_SSID);

	ret = net_mgmt(NET_REQUEST_WIFI_CONNECT,
		       iface,
		       &params,
		       sizeof(params));
	if (ret < 0) {
		LOG_ERR("NET_REQUEST_WIFI_CONNECT fallo: %d", ret);
		return ret;
	}

	while (!wifi_connected && retries-- > 0) {
		k_msleep(1000);
	}

	if (!wifi_connected) {
		return -ETIMEDOUT;
	}

	/* Esperar DHCP/IPv4 preferida. */
	for (int i = 0; i < 30; ++i) {
		struct net_in_addr *addr =
			net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);

		if (addr != NULL) {
			char ip[NET_INET_ADDRSTRLEN];
			net_addr_ntop(NET_AF_INET, addr, ip, sizeof(ip));
			LOG_INF("IPv4 asignada por DHCP: %s", ip);
			return 0;
		}

		k_msleep(500);
	}

	LOG_ERR("Wi-Fi asociado pero no se obtuvo una IPv4 preferida por DHCP");
	return -ETIMEDOUT;
}

/* -------------------------------------------------------------------------- */
/* TCP: conexión ESP32 -> PC                                                  */
/* -------------------------------------------------------------------------- */

static int tcp_connect_to_pc(void)
{
	struct zsock_sockaddr_in server = {0};
	int sock;
	int ret;

	sock = zsock_socket(ZSOCK_AF_INET, ZSOCK_SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0) {
		LOG_ERR("socket() fallo: errno=%d", errno);
		return -errno;
	}

	server.sin_family = ZSOCK_AF_INET;
	server.sin_port = zsock_htons(PC_SERVER_PORT);

	ret = zsock_inet_pton(ZSOCK_AF_INET, PC_SERVER_IP, &server.sin_addr);
	if (ret != 1) {
		LOG_ERR("IP del PC invalida: %s", PC_SERVER_IP);
		(void)zsock_close(sock);
		return -EINVAL;
	}

	LOG_INF("Conectando TCP a %s:%d...", PC_SERVER_IP, PC_SERVER_PORT);

	ret = zsock_connect(sock,
			    (struct zsock_sockaddr *)&server,
			    sizeof(server));
	if (ret < 0) {
		LOG_WRN("connect() fallo: errno=%d", errno);
		(void)zsock_close(sock);
		return -errno;
	}

	k_mutex_lock(&sock_mutex, K_FOREVER);
	tcp_sock = sock;
	k_event_post(&conn_event, CONN_EVENT_CONNECTED);
	k_mutex_unlock(&sock_mutex);

	LOG_INF("TCP conectado correctamente");
	return 0;
}

/* -------------------------------------------------------------------------- */
/* RX event hook -> semáforo binario                                          */
/* -------------------------------------------------------------------------- */

static void rx_event_hook(void)
{
	/*
	 * Punto de sincronización. No ejecuta procesamiento pesado.
	 * El procesamiento real se hace en rx_processing_thread().
	 */
	k_sem_give(&rx_sem);
}

static void rx_enqueue_frame(const char *frame, size_t len)
{
	struct rx_frame item = {0};

	if (len == 0U || len >= MAX_FRAME_SIZE) {
		LOG_ERR("Trama RX demasiado grande: %u bytes", (unsigned int)len);
		return;
	}

	item.len = (uint16_t)len;
	memcpy(item.data, frame, len);
	item.data[len] = '\0';

	if (k_msgq_put(&rx_msgq, &item, K_NO_WAIT) != 0) {
		LOG_ERR("Cola RX llena: trama descartada");
		return;
	}

	rx_event_hook();
}

/* -------------------------------------------------------------------------- */
/* Thread de recepción                                                        */
/* -------------------------------------------------------------------------- */

static void rx_thread(void *p1, void *p2, void *p3)
{
	char stream[MAX_FRAME_SIZE];
	size_t stream_len = 0U;
	uint8_t chunk[64];

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		int sock;
		int ret;

		k_event_wait(&conn_event, CONN_EVENT_CONNECTED, false, K_FOREVER);

		if (!connection_get_socket(&sock)) {
			continue;
		}

		ret = zsock_recv(sock, chunk, sizeof(chunk), 0);
		if (ret <= 0) {
			if (ret == 0) {
				LOG_WRN("TCP: peer cerro la conexión");
			} else {
				LOG_WRN("TCP recv() fallo: errno=%d", errno);
			}

			stream_len = 0U;
			connection_clear();
			k_msleep(100);
			continue;
		}

		for (int i = 0; i < ret; ++i) {
			char c = (char)chunk[i];

			if (c == '\n') {
				/* La trama se entrega sin el salto final. */
				rx_enqueue_frame(stream, stream_len);
				stream_len = 0U;
				continue;
			}

			if (stream_len + 1U >= sizeof(stream)) {
				LOG_ERR("RX: JSON excede MAX_FRAME_SIZE; descartando");
				stream_len = 0U;
				continue;
			}

			stream[stream_len++] = c;
		}
	}
}

/* -------------------------------------------------------------------------- */
/* Construcción y envío de telemetría                                         */
/* -------------------------------------------------------------------------- */

static int send_telemetry(int32_t seq)
{
	struct telemetry_json msg = {
		.type = "telemetry",
		.seq = seq,
		/* El valor cambia cada ciclo para garantizar cambios del CRC. */
		.value = 20 + ((seq * 7) % 61),
		.crc_errors = crc_error_count,
	};

	char body[MAX_FRAME_SIZE];
	char frame[MAX_FRAME_SIZE];
	int ret;

	ret = json_obj_encode_buf(telemetry_descr,
				  ARRAY_SIZE(telemetry_descr),
				  &msg,
				  body,
				  sizeof(body));
	if (ret < 0) {
			LOG_ERR("JSON telemetry encode fallo: %d", ret);
			return ret;
	}

	ret = frame_with_crc(body, strlen(body), frame, sizeof(frame));
	if (ret < 0) {
		return ret;
	}

	LOG_INF("TX telemetry: %s", frame);
	return send_all(frame, (size_t)ret);
}

/* -------------------------------------------------------------------------- */
/* Thread de transmisión periódica                                            */
/* -------------------------------------------------------------------------- */

static void tx_thread(void *p1, void *p2, void *p3)
{
	int32_t seq = 0;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		int sock;

		k_event_wait(&conn_event, CONN_EVENT_CONNECTED, false, K_FOREVER);

		/* Comprueba que la conexión sigue presente antes de transmitir. */
		if (!connection_get_socket(&sock)) {
			continue;
		}

		int ret = send_telemetry(++seq);
		if (ret < 0) {
			LOG_WRN("TX telemetry fallo: %d", ret);
			connection_clear();
			continue;
		}

		k_msleep(TX_PERIOD_MS);
	}
}

/* -------------------------------------------------------------------------- */
/* ACK hacia PC                                                               */
/* -------------------------------------------------------------------------- */

static void send_command_ack(double value)
{
	struct ack_json msg = {
		.type = "ack",
		.status = 0,
		.value = value,
		.crc_errors = crc_error_count,
	};

	char body[MAX_FRAME_SIZE];
	char frame[MAX_FRAME_SIZE];
	int ret;

	ret = json_obj_encode_buf(ack_descr,
				  ARRAY_SIZE(ack_descr),
				  &msg,
				  body,
				  sizeof(body));
	if (ret < 0) {
			LOG_ERR("ACK JSON encode fallo: %d", ret);
			return;
	}

	ret = frame_with_crc(body, strlen(body), frame, sizeof(frame));
	if (ret < 0) {
		LOG_ERR("ACK frame fallo: %d", ret);
		return;
	}

	LOG_INF("TX ACK: %s", frame);
	if (send_all(frame, (size_t)ret) < 0) {
		LOG_WRN("No se pudo enviar ACK");
	}
}

/* -------------------------------------------------------------------------- */
/* Procesamiento de JSON recibido                                             */
/* -------------------------------------------------------------------------- */

static void process_received_frame(const struct rx_frame *item)
{
	char body[MAX_FRAME_SIZE];
	uint16_t received_crc;
	uint16_t calculated_crc;
	int64_t parse_ret;
	int ret;

	ret = parse_crc_frame(item->data,
			      item->len,
			      body,
			      sizeof(body),
			      &received_crc);
	if (ret < 0) {
		LOG_ERR("RX: trama/CRC malformado (ret=%d)", ret);
		crc_error_count++;
		return;
	}

	calculated_crc = crc16_ccitt_false((const uint8_t *)body,
					 strlen(body));

	if (calculated_crc != received_crc) {
		crc_error_count++;
		LOG_ERR("ERROR CRC: recibido=0x%04X calculado=0x%04X total=%u",
			(uint32_t)received_crc,
			(uint32_t)calculated_crc,
			crc_error_count);
		return;
	}

	LOG_INF("RX CRC OK: 0x%04X | body=%s",
		(uint32_t)received_crc, body);

	/* Sólo necesitamos el formato command desde el PC. */
	struct command_json cmd = {0};
	parse_ret = json_obj_parse(body,
				   strlen(body),
				   command_descr,
				   ARRAY_SIZE(command_descr),
				   &cmd);

	if (parse_ret < 0) {
		LOG_ERR("RX JSON invalido: %lld", parse_ret);
		return;
	}

	if (cmd.type == NULL || strcmp(cmd.type, "command") != 0) {
		LOG_WRN("RX JSON ignorado: type no es 'command'");
		return;
	}

	LOG_INF("Comando recibido desde PC: value=%.3f",
		(double)cmd.value);

	/* Se responde para demostrar comunicación bidireccional. */
	send_command_ack(cmd.value);
}

/* -------------------------------------------------------------------------- */
/* Thread de procesamiento: bloqueado en semáforo binario                     */
/* -------------------------------------------------------------------------- */

static void rx_processing_thread(void *p1, void *p2, void *p3)
{
	struct rx_frame item;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		/* BLOQUEO: no consume CPU esperando datos. */
		k_sem_take(&rx_sem, K_FOREVER);

		/*
		 * El semáforo es binario, pero la cola puede contener varias tramas.
		 * Drenamos toda la cola tras una activación para no perder avisos.
		 */
		while (k_msgq_get(&rx_msgq, &item, K_NO_WAIT) == 0) {
			process_received_frame(&item);
		}
	}
}

/* -------------------------------------------------------------------------- */
/* main                                                                       */
/* -------------------------------------------------------------------------- */

int main(void)
{
	int ret;

	LOG_INF("============================================");
	LOG_INF("ESP32 Zephyr - WiFi/TCP/JSON/CRC");
	LOG_INF("Servidor PC: %s:%d", PC_SERVER_IP, PC_SERVER_PORT);
	LOG_INF("============================================");

	/* Registrar eventos Wi-Fi. */
	net_mgmt_init_event_callback(&wifi_cb,
				     wifi_mgmt_handler,
				     NET_EVENT_WIFI_CONNECT_RESULT |
				     NET_EVENT_WIFI_DISCONNECT_RESULT);
	net_mgmt_add_event_callback(&wifi_cb);

	/* Conectar Wi-Fi. */
	while (!wifi_connected) {
		ret = wifi_connect();
		if (ret == 0) {
			break;
		}

		LOG_WRN("Reintentando Wi-Fi en 2 s (ret=%d)", ret);
		k_msleep(2000);
	}

	/* Lanzar threads una vez que la red está disponible. */
	k_thread_create(&rx_thread_data,
			rx_stack,
			K_THREAD_STACK_SIZEOF(rx_stack),
			rx_thread,
			NULL, NULL, NULL,
			5, 0, K_NO_WAIT);
	k_thread_name_set(&rx_thread_data, "tcp_rx");

	k_thread_create(&tx_thread_data,
			tx_stack,
			K_THREAD_STACK_SIZEOF(tx_stack),
			tx_thread,
			NULL, NULL, NULL,
			6, 0, K_NO_WAIT);
	k_thread_name_set(&tx_thread_data, "tcp_tx");

	k_thread_create(&proc_thread_data,
			proc_stack,
			K_THREAD_STACK_SIZEOF(proc_stack),
			rx_processing_thread,
			NULL, NULL, NULL,
			7, 0, K_NO_WAIT);
	k_thread_name_set(&proc_thread_data, "json_proc");

	/* Administrador de conexión TCP: reconecta indefinidamente. */
	while (true) {
		if (!wifi_connected) {
			connection_clear();
			ret = wifi_connect();
			if (ret != 0) {
				k_msleep(2000);
				continue;
			}
		}

		int current_sock;

		if (!connection_get_socket(&current_sock)) {
			int connect_ret = tcp_connect_to_pc();
			if (connect_ret < 0) {
				k_msleep(2000);
				continue;
			}
		}

		k_msleep(500);
	}

	return 0;
}
