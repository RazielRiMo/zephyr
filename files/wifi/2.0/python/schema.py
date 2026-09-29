"""
schema.py
=========
POR QUÉ este archivo existe (mismo objetivo que json_schema.h/c del lado
ESP32): es el ÚNICO lugar de la aplicación de PC donde se describe "de qué
campos se compone cada trama JSON". protocol.py NO tiene ningún nombre de
campo escrito a mano en su lógica de codificación/verificación -- todo pasa
por las funciones de este módulo. Para agregar, quitar o renombrar un campo
en un futuro proyecto, basta con editar las clases de abajo; ni
network_module.py ni gui_module.py necesitan cambiar.

CON QUÉ se comunica: usa exclusivamente el módulo nativo `json` (a través
de protocol.py) -- nunca concatenación de strings ni f-strings para
construir el JSON en sí. Cada clase aquí es, conceptualmente, el mismo rol
que un `struct` + `json_obj_descr[]` en json_schema.h/c: agrupa "qué
campos tiene esta trama y en qué orden".
"""

from dataclasses import dataclass, asdict, fields


@dataclass
class CommandPayload:
    """Campos de una trama de comando (PC -> ESP32), SIN "crc".

    PARÁMETROS de cada campo:
      cmd   -- string fijo "set_value": identifica la intención del
               mensaje (deja espacio para otros "cmd" en el futuro sin
               romper el esquema).
      value -- entero escrito por el usuario en el campo de texto de la
               GUI; es el dato que la ESP32 usará en apply_local_action().

    El ORDEN de los campos aquí declarados es el orden en que se
    serializan en el JSON (ver protocol.py: usa asdict(), que preserva el
    orden de declaración de un dataclass) -- debe coincidir con el orden
    de campos en json_schema.h (struct command_payload) para que el CRC
    calculado en la ESP32 y el recalculado en la PC operen sobre bytes
    equivalentes.
    """
    cmd: str
    value: int


@dataclass
class TelemetryPayload:
    """Campos de una trama de telemetría (ESP32 -> PC), SIN "crc".

    Se usa principalmente como REFERENCIA/documentación: la PC solo
    DECODIFICA telemetría (nunca la construye), así que
    parse_and_validate_frame() en protocol.py no necesita instanciar esta
    clase -- pero mantenerla aquí, con el mismo orden de campos que
    json_schema.h (struct telemetry_payload), documenta el contrato del
    protocolo en un solo lugar para quien lea el proyecto.
    """
    type: str
    seq: int
    uptime_ms: int
    value: int
    esp32_crc_errors: int


def field_names(schema_cls) -> list:
    """Devuelve, en orden, los nombres de campo de una dataclass del
    esquema. Utilidad pequeña para que protocol.py pueda, si hace falta,
    validar que un dict recibido tiene exactamente los campos esperados
    sin repetir los nombres a mano en dos lugares distintos."""
    return [f.name for f in fields(schema_cls)]


def payload_dict(payload) -> dict:
    """Convierte una instancia de CommandPayload/TelemetryPayload a un
    dict ORDINARIO, preservando el orden de declaración de sus campos
    (asdict() de dataclasses lo garantiza). Es lo que protocol.py pasa a
    json.dumps()."""
    return asdict(payload)
