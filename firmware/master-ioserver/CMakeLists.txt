#
# FRANK master I/O server.
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# SPDX-License-Identifier: GPL-3.0-or-later
#
cmake_minimum_required(VERSION 3.13)

set(FW_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(HWTESTS_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../hwtests)

set(PICO_BOARD_HEADER_DIRS ${HWTESTS_ROOT}/boards CACHE PATH "" FORCE)
# This only ever runs on the master: the peripherals it serves are all on that
# half, and it needs the RP2350B's 48-GPIO window for the link's upper pins.
set(PICO_BOARD "frank_core2_master" CACHE STRING "Pico board type" FORCE)

set(CPU_SPEED   "252" CACHE STRING "CPU clock in MHz")
set(PSRAM_SPEED "133" CACHE STRING "PSRAM max frequency in MHz")
set(FLASH_SPEED "66"  CACHE STRING "Flash max frequency in MHz")

include(${HWTESTS_ROOT}/pico_sdk_import.cmake)
project(master-ioserver C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

# DispHSTX (DVI over the HSTX peripheral), vendored from Protea.
add_subdirectory(lib/DispHSTX DispHSTX)

add_executable(master-ioserver
    src/main.c
    src/storage.c
    src/display_tick.c
    term/display.c
    term/cellbuf.c
    term/textmode_render.c
    term/font.c
    term/palette.c
    term/framebuf.c
    term/terminal.c
    term/term_compat.c
    sdcard/sdcard.c
    ${FW_ROOT}/common/link_bus.c
)

pico_generate_pio_header(master-ioserver ${FW_ROOT}/common/link_bus.pio)

target_include_directories(master-ioserver PRIVATE src sdcard term ${FW_ROOT}/common)

# DispHSTX reads its configuration from whatever is force-included ahead of its
# own _config.h; there is no other way to override it.
target_compile_options(master-ioserver PRIVATE
    -include ${CMAKE_CURRENT_LIST_DIR}/term/config.h)

target_compile_definitions(master-ioserver PRIVATE
    CPU_SPEED=${CPU_SPEED}
    # microSD on J7. The vendored driver defaults to another board's pins.
    SDCARD_PIN_SPI0_SCK=6
    SDCARD_PIN_SPI0_MOSI=7
    SDCARD_PIN_SPI0_MISO=4
    SDCARD_PIN_SPI0_CS=5
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
    FLASH_MAX_FREQ_MHZ=${FLASH_SPEED}
)

target_compile_options(master-ioserver PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)

target_link_libraries(master-ioserver
    DispHSTX
    pico_stdlib
    hardware_clocks
    hardware_dma
    hardware_gpio
    hardware_pio
    hardware_spi
    hardware_sync
    hardware_vreg
)

# Console on UART0 (J2), read through the master's probe.
pico_enable_stdio_uart(master-ioserver 1)
pico_enable_stdio_usb(master-ioserver 0)

pico_add_extra_outputs(master-ioserver)
