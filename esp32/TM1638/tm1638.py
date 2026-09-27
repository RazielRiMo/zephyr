"""
Libreria simple para el modulo TM1638 (8 displays de 7 segmentos,
8 LEDs, 8 botones) en MicroPython, usando bit-banging con machine.Pin.
No depende de ninguna libreria externa.

A diferencia de una libreria en C con estado global, aqui se modela
como una clase: cada TM1638() que instancias es un modulo fisico
independiente, con sus propios pines y su propio estado de antirrebote.
Esto permite manejar varios modulos TM1638 a la vez si hiciera falta
(bastaria con crear varias instancias con pines distintos).

Uso basico:

    from tm1638 import TM1638
    tm = TM1638(stb=4, clk=18, dio=19)
    tm.display(1234)

Codigos de retorno vs excepciones: en vez de imitar el estilo de C
(0 / -EINVAL / -EIO), los argumentos fuera de rango lanzan ValueError,
que es el estilo idiomatico de Python -- ver el README para el detalle
de que excepciones lanza cada metodo.
"""

from machine import Pin #type: ignore
import time

_DEBOUNCE_MS = 30
_MAX_VALUE = 99999999

# Segmentos a-g para digitos 0-9, usados por display().
_DIGIT_FONT = (
	0x3F, 0x06, 0x5B, 0x4F, 0x66,
	0x6D, 0x7D, 0x07, 0x7F, 0x6F,
)


class TM1638:
	"""
	--- Protocolo del TM1638 (resumen) ---

	3 lineas:
	  STB  chip select. En reposo alto; en bajo mientras dura una
	       transaccion (uno o mas bytes).
	  CLK  reloj generado por nosotros (el maestro). En reposo alto.
	  DIO  datos, BIDIRECCIONAL: la manejamos como salida para escribir
	       comandos/datos, y la reconfiguramos como entrada solo durante
	       los 4 bytes que el TM1638 responde al leer el teclado.

	Cada byte va LSB primero; DIO cambia con CLK en bajo y se
	muestrea/valida con CLK en alto.

	Comandos usados:
	  0x44        Modo de escritura con direccion fija: el siguiente
	              byte fija la direccion (0xC0|addr) y el que sigue a
	              ese es el dato para esa direccion.
	  0xC0 | addr Fija la direccion (0-15) de la memoria de display
	              para el byte de datos que sigue.
	  0x80 | n    Control de pantalla: bit3 = encendida/apagada,
	              bits 0-2 = brillo (0-7).
	  0x42        Inicia lectura del teclado: tras enviar este byte con
	              STB en bajo, se pone DIO en entrada y se leen 4 bytes.

	La memoria de display tiene 16 direcciones (0-15). En los modulos
	"LED&KEY" estandar (8 digitos + 8 LEDs + 8 botones en fila), cada
	digito i vive en la direccion par i*2 y su LED asociado en la
	direccion impar i*2+1.
	"""

	def __init__(self, stb, clk, dio, brightness=4):
		"""
		stb, clk, dio: numeros de pin GPIO del ESP32 (enteros).
		brightness: nivel inicial de brillo, 0-7 (por defecto 4).
		"""
		self._stb = Pin(stb, Pin.OUT, value=1)
		self._clk = Pin(clk, Pin.OUT, value=1)
		self._dio = Pin(dio, Pin.OUT, value=1)

		self._keys_raw_prev = 0
		self._keys_stable = 0
		self._keys_change_ts = time.ticks_ms() #type: ignore

		self.clear()
		self.set_brightness(brightness)

	# ---------------- bajo nivel (privado) ----------------

	def _dio_out(self):
		self._dio.init(Pin.OUT)

	def _dio_in(self):
		# Pull-up como red de seguridad; el TM1638 maneja DIO
		# activamente al responder, pero esto evita lecturas
		# flotantes si algo falla en el cableado.
		self._dio.init(Pin.IN, Pin.PULL_UP)

	def _send_byte(self, b):
		self._dio_out()
		for i in range(8):
			self._clk.value(0)
			self._dio.value((b >> i) & 0x01)
			time.sleep_us(1) #type: ignore
			self._clk.value(1)
			time.sleep_us(1) #type: ignore

	def _recv_byte(self):
		b = 0
		self._dio_in()
		for i in range(8):
			self._clk.value(0)
			time.sleep_us(1)#type: ignore
			if self._dio.value():
				b |= (1 << i)
			self._clk.value(1)
			time.sleep_us(1)#type: ignore
		return b

	def _write_at(self, addr, data):
		self._stb.value(0)
		self._send_byte(0x44)
		self._stb.value(1)
		time.sleep_us(1)#type: ignore

		self._stb.value(0)
		self._send_byte(0xC0 | (addr & 0x0F))
		self._send_byte(data & 0xFF)
		self._stb.value(1)

	def _scan_keys_raw(self):
		buf = bytearray(4)

		self._stb.value(0)
		self._send_byte(0x42)
		for i in range(4):
			buf[i] = self._recv_byte()
		self._stb.value(1)
		self._dio_out()

		# Agrupacion real de este modulo (verificada empiricamente):
		# buf[i] bit0 -> boton (i+1)  [botones 1-4]
		# buf[i] bit4 -> boton (i+5)  [botones 5-8]
		# (no es buf[i] bit0/bit4 -> botones (2i+1)/(2i+2) como en la
		# version anterior -- esa agrupacion daba impares/pares cruzados).
		keys = 0
		for i in range(4):
			if buf[i] & 0x01:
				keys |= (1 << i)
			if buf[i] & 0x10:
				keys |= (1 << (i + 4))
		return keys

	# ---------------- API publica ----------------

	def set_brightness(self, level):
		"""Ajusta el brillo. level: 0 (minimo) a 7 (maximo)."""
		if not 0 <= level <= 7:
			raise ValueError("level debe estar entre 0 y 7")

		self._stb.value(0)
		self._send_byte(0x88 | level)  # bit3=1 -> pantalla encendida
		self._stb.value(1)

	def set_digit(self, digit, segments):
		"""
		Control manual de los segmentos de un digito.
		digit: 0-7 (0 = digito mas a la izquierda, 7 = mas a la
		       derecha).
		segments: mapa de bits, uno por segmento (orden real del
		          TM1638, no hace falta reordenarlo):
		    bit 0 = a      bit 4 = e
		    bit 1 = b      bit 5 = f
		    bit 2 = c      bit 6 = g
		    bit 3 = d      bit 7 = dp (punto decimal)
		    0xFF enciende los 7 segmentos + el punto decimal.
		    0x00 apaga todos los segmentos de ese digito.
		"""
		if not 0 <= digit <= 7:
			raise ValueError("digit debe estar entre 0 y 7")

		self._write_at(digit * 2, segments)

	def display(self, value):
		"""
		Muestra un numero entero en los 8 digitos de 7 segmentos,
		alineado a la derecha (el digito de la derecha = unidades).
		Los digitos no usados quedan apagados. value = 0 se muestra
		correctamente como "0".

		Rango valido: 0 a 99999999 (8 digitos). Fuera de rango
		(incluye negativos): se dibuja una fila de guiones
		"--------" en el modulo (para que el error tambien sea
		visible en el hardware) y se lanza ValueError.
		"""
		if not 0 <= value <= _MAX_VALUE:
			for pos in range(8):
				self.set_digit(pos, 0x40)
			raise ValueError("value debe estar entre 0 y 99999999")

		self.clear_digits()

		pos = 7
		v = value
		while True:
			self.set_digit(pos, _DIGIT_FONT[v % 10])
			v //= 10
			pos -= 1
			if v == 0 or pos < 0:
				break

	def clear(self):
		"""Apaga los 8 digitos de 7 segmentos Y los 8 LEDs."""
		for addr in range(16):
			self._write_at(addr, 0x00)

	def clear_digits(self):
		"""Apaga solo los 8 digitos de 7 segmentos (los LEDs no se tocan)."""
		for pos in range(8):
			self._write_at(pos * 2, 0x00)

	def clear_leds(self):
		"""Apaga solo los 8 LEDs (los digitos no se tocan)."""
		for pos in range(8):
			self._write_at(pos * 2 + 1, 0x00)

	def set_led(self, led, state):
		"""
		Enciende o apaga un LED individual.
		led: 0-7. state: 0 (apagado) o 1 (encendido).
		"""
		if not 0 <= led <= 7:
			raise ValueError("led debe estar entre 0 y 7")
		if state not in (0, 1):
			raise ValueError("state debe ser 0 o 1")

		self._write_at(led * 2 + 1, 0x01 if state else 0x00)

	def get_button(self):
		"""
		Devuelve que boton esta presionado, con antirrebote ya
		aplicado:
		   0     -> ningun boton presionado
		  1..8   -> boton N presionado (numerados de izquierda a
		            derecha)

		Si se presiona mas de un boton a la vez, se devuelve el de
		menor numero (el mas a la izquierda) -- es la unica forma de
		representar el resultado con un solo entero.

		El antirrebote es NO bloqueante (no usa time.sleep interno):
		una lectura solo se toma como valida despues de que el
		estado fisico se mantiene estable por 30 ms. Por eso hay que
		llamar a este metodo periodicamente (por ejemplo cada 10-20
		ms dentro del loop principal).
		"""
		raw = self._scan_keys_raw()
		now = time.ticks_ms()#type: ignore

		if raw != self._keys_raw_prev:
			self._keys_raw_prev = raw
			self._keys_change_ts = now
		elif time.ticks_diff(now, self._keys_change_ts) >= _DEBOUNCE_MS:#type: ignore
			self._keys_stable = raw

		if self._keys_stable == 0:
			return 0

		for i in range(8):
			if self._keys_stable & (1 << i):
				return i + 1

		return 0