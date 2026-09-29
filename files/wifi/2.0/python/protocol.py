"""
protocol.py
===========
POR QUÉ este módulo existe: concentra la lógica de "cómo se firma y
verifica una trama con CRC-16" -- el mismo patrón para cualquier tipo de
trama, sin importar qué campos tenga (eso vive en schema.py). Es el
equivalente Python de json_protocol.c del lado ESP32.

Requisito obligatorio del proyecto: NINGUNA función de este archivo arma o
interpreta JSON con concatenación de strings o f-strings sobre el TEXTO
completo del mensaje -- todo pasa por `json.dumps` / `json.loads` (el
módulo nativo de Python). La única concatenación de texto que aparece
aquí es el '\\n' final, que es framing de TRANSPORTE (delimitador de
mensaje sobre un socket TCP), no contenido JSON.

CON QUÉ se comunica: lo usa network_module.py, tanto para construir la
trama de comando que se envía a la ESP32 como para decodificar/verificar
cada línea de telemetría que llega desde ella.
"""

import json

from schema import CommandPayload, payload_dict

# PARÁMETRO: separadores COMPACTOS (sin espacio tras ':' ni ',').
# Por qué es crítico fijarlo explícitamente: el valor por defecto de
# json.dumps() en Python SÍ incluye espacios (', ' y ': '), mientras que
# <data/json.h> de Zephyr serializa en formato compacto. Si un lado
# incluyera espacios y el otro no, el CRC (que se calcula sobre los BYTES
# exactos de la serialización) jamás coincidiría entre ambos lenguajes.
JSON_SEPARATORS = (",", ":")


def crc16_ccitt_false(data: bytes) -> int:
    """CRC-16/CCITT-FALSE: poly=0x1021, init=0xFFFF, sin reflejar, sin XOR
    final -- misma implementación bit a bit que crc16.c, para que ambos
    lados calculen siempre el mismo valor sobre los mismos bytes.

    Vector de prueba (para validar cualquier reimplementación, incluida la
    de crc16.c): crc16_ccitt_false(b"123456789") == 0x29B1

    PARÁMETRO data: bytes crudos a proteger (normalmente, la salida de
    json.dumps() ya codificada en utf-8) -- no un str, para dejar
    explícito que el cálculo opera sobre la representación en bytes, tal
    como ocurre en el lado C.
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


def _sign_and_serialize(payload: dict) -> str:
    """Función CENTRAL y genérica: recibe un dict de datos (SIN "crc"),
    lo serializa con json.dumps, calcula su CRC-16 y devuelve el JSON
    final (CON "crc") como texto. No sabe nada de "cmd" ni "value" en
    particular -- cualquier trama de este protocolo (actual o futura)
    puede construirse llamando a esta función, sin tocar su lógica.

    POR QUÉ separar esta función de build_command_frame(): así, si mañana
    se agrega un segundo tipo de trama saliente, su función específica
    puede reutilizar exactamente esta misma lógica de firmado.
    """
    # PASO 1: serializar SOLO los datos (sin "crc") -- json.dumps() es la
    # ÚNICA responsable de producir el texto JSON; nunca se concatenan
    # las claves/valores a mano.
    payload_json = json.dumps(payload, separators=JSON_SEPARATORS)

    # PASO 2: CRC-16 sobre esos bytes EXACTOS.
    crc = crc16_ccitt_false(payload_json.encode("utf-8"))

    # PASO 3: agregar "crc" al dict (como STRING de 4 hex, igual que en
    # json_schema.h, para conservar ceros a la izquierda) y volver a
    # serializar con json.dumps -- de nuevo, nunca a mano. Como los dicts
    # de Python (3.7+) preservan el orden de inserción y json.dumps
    # serializa en ese orden, el resultado es exactamente
    # payload_json + `,"crc":"XXXX"}` en vez de `}` -- aunque aquí eso lo
    # logra la librería, no una operación de texto manual.
    full = dict(payload)
    full["crc"] = f"{crc:04X}"

    return json.dumps(full, separators=JSON_SEPARATORS)


def build_command_frame(value: int) -> bytes:
    """Arma la trama de comando {"cmd":"set_value","value":N,"crc":"XXXX"}
    siguiendo el esquema CommandPayload (schema.py) y la firma con CRC-16.

    COMUNICACIÓN: la usa network_module.py cada vez que la GUI encola un
    valor para enviar a la ESP32.

    PARÁMETRO value: entero tecleado por el usuario en la GUI; se castea
    con int() por seguridad (defensivo: si algún llamador futuro pasara un
    str numérico, no rompe la serialización).
    """
    payload = payload_dict(CommandPayload(cmd="set_value", value=int(value)))
    frame_json = _sign_and_serialize(payload)

    # Framing por línea: TCP es un flujo de bytes sin límites de mensaje
    # propios -- se agrega '\n' como delimitador de TRANSPORTE (no
    # contenido JSON) para que la ESP32 sepa dónde termina esta trama.
    return (frame_json + "\n").encode("utf-8")


class FrameError(Exception):
    """La línea recibida no se pudo interpretar como JSON, o le falta el
    campo "crc"."""


def parse_and_validate_frame(line: str) -> dict:
    """Decodifica CUALQUIER trama de este protocolo (usa exclusivamente
    json.loads) y verifica su CRC re-serializando los campos ya
    decodificados (sin "crc") con el mismo formato compacto -- sin
    importar qué campos tenga el esquema de esa trama en particular.

    COMUNICACIÓN: la usa network_module.py por cada línea completa
    ('\\n'-terminada) que llega desde el socket de la ESP32.

    Lanza FrameError si 'line' no es JSON válido o no tiene "crc". Si el
    JSON es válido pero el CRC no coincide, IGUAL devuelve el dict
    (con "_crc_ok" en False) -- eso permite a network_module.py decidir
    contar el error y avisar, sin descartar silenciosamente el intento.
    """
    try:
        # Única llamada de DECODIFICACIÓN de este módulo: json.loads es
        # quien realmente interpreta la sintaxis JSON. 'data' conserva el
        # orden de aparición de las claves en 'line' (json.loads no
        # reordena), lo cual es exactamente lo que se necesita en el
        # siguiente paso.
        data = json.loads(line)
    except json.JSONDecodeError as exc:
        raise FrameError(f"JSON invalido: {exc}") from exc

    if not isinstance(data, dict) or "crc" not in data:
        raise FrameError(f"falta el campo \"crc\" en la trama: {line!r}")

    received_crc = int(data["crc"], 16)

    # Reconstruye el payload "sin crc" a partir del MISMO dict decodificado
    # (no del texto crudo): como 'data' preserva el orden original de
    # 'line', este diccionario re-serializa exactamente igual que como lo
    # serializó quien lo envió (la ESP32, vía <data/json.h>) ANTES de
    # agregar su propio campo "crc".
    payload = {k: v for k, v in data.items() if k != "crc"}
    payload_json = json.dumps(payload, separators=JSON_SEPARATORS)
    computed_crc = crc16_ccitt_false(payload_json.encode("utf-8"))

    data["_crc_ok"] = received_crc == computed_crc
    data["_crc_received"] = received_crc
    data["_crc_computed"] = computed_crc

    return data
