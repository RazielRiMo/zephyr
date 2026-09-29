"""
protocol.py
-----------
Definicion del protocolo de tramas JSON + CRC-16 compartido con el
firmware de la ESP32 (ver json_protocol.c / crc16.c en firmware_zephyr/src).
Cualquier cambio de formato debe reflejarse en AMBOS lados.

Formato de trama (una por linea, terminada en '\n'):

    {...campos...,"crc":"XXXX"}\n

El CRC (CRC-16/CCITT-FALSE: poly=0x1021, init=0xFFFF, sin reflejar, sin XOR
de salida) se calcula sobre los bytes del objeto JSON tal como quedaria SIN
el campo "crc" -- es decir, cerrado con '}' justo donde empieza ,"crc":"...".
Esto evita depender de una re-serializacion JSON identica byte a byte entre
C y Python: en vez de eso, se recorta la cadena cruda en un punto conocido.
"""

import json

CRC_MARKER = ',"crc":"'


def crc16_ccitt_false(data: bytes) -> int:
    """CRC-16/CCITT-FALSE: poly=0x1021, init=0xFFFF, sin reflejar, sin XOR final.

    Vector de prueba (para validar cualquier reimplementacion, incluida la
    de crc16.c): crc16_ccitt_false(b"123456789") == 0x29B1
    """
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def build_command_frame(value: int) -> bytes:
    """Construye una trama de comando: {"cmd":"set_value","value":N,"crc":"XXXX"}\n."""
    payload = f'{{"cmd":"set_value","value":{int(value)}}}'
    crc = crc16_ccitt_false(payload.encode("utf-8"))
    frame = payload[:-1] + f',"crc":"{crc:04X}"}}\n'
    return frame.encode("utf-8")


class FrameError(Exception):
    """La linea recibida no tiene el formato minimo esperado (falta "crc")."""


def parse_and_validate_frame(line: str) -> dict:
    """Valida el CRC de una linea recibida y devuelve su contenido como dict.

    Lanza FrameError si no se encuentra el campo "crc" (trama corrupta o de
    otro formato). Si el campo SI esta presente pero el CRC no coincide,
    igual devuelve el dict parseado, con "_crc_ok" en False, para que el
    llamador decida que hacer (aqui: contarlo como error y avisar, sin
    frenar la aplicacion).
    """
    idx = line.find(CRC_MARKER)

    if idx == -1:
        raise FrameError(f"no se encontro el campo \"crc\" en la trama: {line!r}")

    crc_hex = line[idx + len(CRC_MARKER): idx + len(CRC_MARKER) + 4]

    if len(crc_hex) != 4:
        raise FrameError(f"campo crc incompleto en la trama: {line!r}")

    protected = line[:idx] + "}"
    received_crc = int(crc_hex, 16)
    computed_crc = crc16_ccitt_false(protected.encode("utf-8"))

    data = json.loads(protected)
    data["_crc_ok"] = received_crc == computed_crc
    data["_crc_received"] = received_crc
    data["_crc_computed"] = computed_crc

    return data
