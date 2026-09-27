"""Servidor TCP y protocolo JSON+CRC para el proyecto ESP32/Zephyr.

El servidor corre en su propio hilo. La GUI nunca hace recv() directamente;
los eventos de red son enviados a una queue.Queue para que Tkinter los lea
periódicamente desde el hilo principal.
"""

from __future__ import annotations

import json
import queue
import socket
import threading
from dataclasses import dataclass
from typing import Any

# ---------------------------------------------------------------------------
# Configuración PC
# ---------------------------------------------------------------------------

LISTEN_HOST = "0.0.0.0"
LISTEN_PORT = 5000
MAX_FRAME_SIZE = 4096
SOCKET_BACKLOG = 1

# CRC-16-CCITT-FALSE: poly=0x1021, init=0xFFFF, xorout=0x0000.

def crc16_ccitt_false(data: bytes) -> int:
    """Calcula el CRC-16-CCITT-FALSE sobre bytes exactos."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def build_frame(payload: dict[str, Any]) -> bytes:
    """Construye JSON sin espacios + CRC + salto de línea.

    El CRC se calcula sobre el JSON sin el campo 'crc'.
    """
    body = json.dumps(
        payload,
        ensure_ascii=False,
        separators=(",", ":"),
    )
    body_bytes = body.encode("utf-8")
    crc = crc16_ccitt_false(body_bytes)
    frame = body[:-1] + f',"crc":{crc}}}\n'
    return frame.encode("utf-8")


def validate_frame(line: bytes) -> tuple[dict[str, Any], int, int]:
    """Valida CRC y devuelve (JSON sin crc, crc_recibido, crc_calculado).

    Importante: se conserva el body textual exacto recibido para que el CRC
    no dependa de reserializar el JSON.
    """
    text = line.decode("utf-8").rstrip("\r\n")
    marker = ',"crc":'
    idx = text.rfind(marker)

    if not text.startswith("{") or not text.endswith("}") or idx < 0:
        raise ValueError("Trama JSON/CRC malformada")

    body = text[:idx] + "}"
    crc_text = text[idx + len(marker) : -1]

    if not crc_text.isdigit():
        raise ValueError("CRC no numérico")

    received_crc = int(crc_text)
    if not 0 <= received_crc <= 0xFFFF:
        raise ValueError("CRC fuera de rango")

    body_bytes = body.encode("utf-8")
    calculated_crc = crc16_ccitt_false(body_bytes)

    if calculated_crc != received_crc:
        raise CrcError(received_crc, calculated_crc, body)

    payload = json.loads(body)
    if not isinstance(payload, dict):
        raise ValueError("El JSON debe ser un objeto")

    return payload, received_crc, calculated_crc


class CrcError(ValueError):
    """Indica una falla de CRC e incluye ambos valores."""

    def __init__(self, received: int, calculated: int, body: str) -> None:
        super().__init__(
            f"CRC incorrecto: recibido=0x{received:04X}, "
            f"calculado=0x{calculated:04X}"
        )
        self.received = received
        self.calculated = calculated
        self.body = body


@dataclass(slots=True)
class NetworkEvent:
    """Evento que consume la GUI."""

    kind: str
    payload: dict[str, Any] | None = None
    message: str = ""


class TcpServer:
    """Servidor TCP concurrente y tolerante a fragmentación de TCP."""

    def __init__(self) -> None:
        self.events: queue.Queue[NetworkEvent] = queue.Queue()
        self._thread = threading.Thread(
            target=self._server_loop,
            name="tcp-server",
            daemon=True,
        )
        self._stop = threading.Event()
        self._client_lock = threading.Lock()
        self._client: socket.socket | None = None
        self.crc_error_count = 0

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self.close_client()

    def send_numeric(self, value: float) -> None:
        """Envía un comando numérico JSON al ESP32."""
        payload = {
            "type": "command",
            "value": value,
        }
        data = build_frame(payload)

        with self._client_lock:
            client = self._client
            if client is None:
                raise ConnectionError("No hay ESP32 conectado")

            client.sendall(data)

        self.events.put(
            NetworkEvent(
                kind="tx",
                payload=payload,
                message=f"TX -> ESP32: {data.decode().rstrip()}"
            )
        )

    def close_client(self) -> None:
        with self._client_lock:
            client = self._client
            self._client = None

        if client is not None:
            try:
                client.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                client.close()
            except OSError:
                pass

    def _set_client(self, client: socket.socket) -> None:
        with self._client_lock:
            old = self._client
            self._client = client

        if old is not None:
            try:
                old.close()
            except OSError:
                pass

    def _server_loop(self) -> None:
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
                server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                server.bind((LISTEN_HOST, LISTEN_PORT))
                server.listen(SOCKET_BACKLOG)
                server.settimeout(1.0)

                self.events.put(
                    NetworkEvent(
                        kind="status",
                        message=f"Servidor TCP escuchando en {LISTEN_HOST}:{LISTEN_PORT}"
                    )
                )

                while not self._stop.is_set():
                    try:
                        client, addr = server.accept()
                    except socket.timeout:
                        continue
                    except OSError as exc:
                        if not self._stop.is_set():
                            self.events.put(
                                NetworkEvent(
                                    kind="error",
                                    message=f"accept() fallo: {exc}"
                                )
                            )
                        break

                    client.settimeout(1.0)
                    self._set_client(client)
                    self.events.put(
                        NetworkEvent(
                            kind="connected",
                            message=f"ESP32 conectado desde {addr[0]}:{addr[1]}"
                        )
                    )

                    self._client_loop(client, addr)

        except OSError as exc:
            self.events.put(
                NetworkEvent(
                    kind="error",
                    message=f"No se pudo iniciar servidor TCP: {exc}"
                )
            )

    def _client_loop(self, client: socket.socket, addr: tuple[str, int]) -> None:
        buffer = bytearray()

        try:
            while not self._stop.is_set():
                try:
                    chunk = client.recv(1024)
                except socket.timeout:
                    continue

                if not chunk:
                    self.events.put(
                        NetworkEvent(
                            kind="disconnected",
                            message=f"ESP32 desconectado ({addr[0]}:{addr[1]})"
                        )
                    )
                    return

                buffer.extend(chunk)

                if len(buffer) > MAX_FRAME_SIZE * 8:
                    self.events.put(
                        NetworkEvent(
                            kind="error",
                            message="Buffer TCP excesivo; se descarta el stream"
                        )
                    )
                    buffer.clear()
                    continue

                while b"\n" in buffer:
                    raw_line, _, rest = buffer.partition(b"\n")
                    buffer = bytearray(rest)

                    if not raw_line:
                        continue

                    try:
                        payload, received_crc, calculated_crc = validate_frame(raw_line)
                    except CrcError as exc:
                        self.crc_error_count += 1
                        # Requisito: alerta en consola.
                        print(
                            f"[ALERTA CRC] ESP32 -> PC: {exc}; "
                            f"errores acumulados={self.crc_error_count}"
                        )
                        self.events.put(
                            NetworkEvent(
                                kind="crc_error",
                                message=(
                                    f"CRC incorrecto: RX=0x{exc.received:04X} "
                                    f"CALC=0x{exc.calculated:04X} "
                                    f"(total={self.crc_error_count})"
                                )
                            )
                        )
                    except (ValueError, UnicodeDecodeError, json.JSONDecodeError) as exc:
                        self.events.put(
                            NetworkEvent(
                                kind="error",
                                message=f"Trama invalida: {exc}"
                            )
                        )
                    else:
                        self.events.put(
                            NetworkEvent(
                                kind="rx",
                                payload=payload,
                                message=(
                                    f"RX <- ESP32 CRC OK 0x{received_crc:04X}: "
                                    f"{json.dumps(payload, ensure_ascii=False)}"
                                ),
                            )
                        )

        except OSError as exc:
            if not self._stop.is_set():
                self.events.put(
                    NetworkEvent(
                        kind="error",
                        message=f"recv() fallo: {exc}"
                    )
                )
        finally:
            with self._client_lock:
                if self._client is client:
                    self._client = None
            try:
                client.close()
            except OSError:
                pass

            self.events.put(
                NetworkEvent(
                    kind="disconnected",
                    message=f"Conexión finalizada con {addr[0]}:{addr[1]}"
                )
            )
