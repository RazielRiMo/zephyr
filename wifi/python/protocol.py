import json

class FrameError(Exception):
    pass

# 8. Comando: def crc_dinamico
# El porqué: Proveer un algoritmo que escanea todas las variables enteras y genera la firma matemática dinámicamente. Al usar XOR el orden no importa, haciendo el JSON verdaderamente flexible.
# La comunicación: Mismo algoritmo de integridad de datos matemáticos que Zephyr.
# Los parámetros: payload_dict (Diccionario que contiene todos los campos extraídos del JSON).
def crc_dinamico(payload_dict: dict) -> int:
    crc = 0
    for key, val in payload_dict.items():
        if key != "crc" and isinstance(val, int):
            crc ^= (val & 0xFFFF)
    return crc & 0xFFFF

def build_command_frame(value: int) -> bytes:
    # Construcción dinámica: Al añadir más variables aquí, el CRC se adaptará solo.
    data = {
        "comando_led": int(value),
        "setpoint_temp": 24, # Variable extra para demostrar flexibilidad
    }
    
    # 9. Comando: json.dumps
    # El porqué: Serialización de los campos dinámicos a JSON de forma nativa.
    data["crc"] = crc_dinamico(data)
    frame = json.dumps(data) + "\n"
    return frame.encode("utf-8")

def parse_and_validate_frame(line: str) -> dict:
    try:
        # 10. Comando: json.loads
        # El porqué: Interpreta nativamente los bytes TCP entrantes, fallando rápidamente si hay bytes corruptos en la estructura del frame.
        data = json.loads(line)
        
        received_crc = data.get("crc", 0)
        computed_crc = crc_dinamico(data)
        
        data["_crc_ok"] = (received_crc == computed_crc)
        data["_crc_received"] = received_crc
        data["_crc_computed"] = computed_crc
        
        return data
    except json.JSONDecodeError as e:
        raise FrameError(f"Error parseando JSON: {e}")
