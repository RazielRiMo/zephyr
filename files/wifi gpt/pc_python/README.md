# PC Python — GUI Tkinter + servidor TCP + JSON + CRC

## Ejecución

```powershell
python gui.py
```

La aplicación queda escuchando en `0.0.0.0:5000`.

Si Windows Firewall pregunta por acceso a Python, permita el tráfico TCP de red privada o cree una regla para el puerto 5000.

## Protocolo

Cada trama termina en `\n`:

```text
{"type":"telemetry","seq":1,"value":27,"crc":12345}\n
```

El CRC-16-CCITT-FALSE se calcula solamente sobre:

```text
{"type":"telemetry","seq":1,"value":27}
```

Esto evita que el campo CRC se incluya a sí mismo en el cálculo.

## Hilos

- `tcp-server`: socket TCP, `recv()`, separación de stream y validación CRC.
- Hilo principal de Tkinter: consume `queue.Queue` y actualiza widgets.

No se actualiza Tkinter directamente desde el hilo de red.
