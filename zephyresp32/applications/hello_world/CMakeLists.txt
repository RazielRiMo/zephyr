# CMakeLists.txt
# Proyecto Zephyr RTOS: puente TCP/JSON con verificación CRC-16 para ESP32.
# Arquitectura de 3 tareas: notificador TCP (rol de ISR), procesador RX
# (bloqueado en un semáforo binario) y emisor periódico de telemetría.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp32_tcp_json_bridge)

target_sources(app PRIVATE
    src/main.c
    src/wifi_manager.c
    src/tcp_server.c
    src/rx_processor.c
    src/telemetry_tx.c
    src/json_protocol.c
    src/crc16.c
)

target_include_directories(app PRIVATE src)
