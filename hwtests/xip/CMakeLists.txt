#
# Phase 2 hardware test: XIP execution, cache coherence and interrupt latency.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
cmake_minimum_required(VERSION 3.13)

set(HWTESTS_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

set(PICO_BOARD_HEADER_DIRS ${HWTESTS_ROOT}/boards CACHE PATH "" FORCE)
set(PICO_BOARD "frank_core2_master" CACHE STRING "Pico board type" FORCE)

set(CPU_SPEED   "252" CACHE STRING "CPU clock in MHz")
set(PSRAM_SPEED "133" CACHE STRING "PSRAM max frequency in MHz")
set(FLASH_SPEED "66"  CACHE STRING "Flash max frequency in MHz")

include(${HWTESTS_ROOT}/pico_sdk_import.cmake)
project(hwtest-xip C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(hwtest-xip
    src/main.c
    ${HWTESTS_ROOT}/common/psram_init.c
)

target_include_directories(hwtest-xip PRIVATE
    src
    ${HWTESTS_ROOT}/common
)

target_compile_definitions(hwtest-xip PRIVATE
    CPU_SPEED=${CPU_SPEED}
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
    FLASH_MAX_FREQ_MHZ=${FLASH_SPEED}
)

# -O1. The measurement loops must not be optimised away or hoisted out of the
# timed region, and the interrupt-latency test depends on the foreground
# actually performing its loads rather than the compiler proving they are dead.
target_compile_options(hwtest-xip PRIVATE -O1 -Wall -Wextra -Wno-unused-parameter)

target_link_libraries(hwtest-xip
    pico_stdlib
    hardware_clocks
    hardware_gpio
    hardware_dma
    hardware_irq
    hardware_sync
    hardware_timer
    hardware_vreg
    hardware_xip_cache
)

# Console on UART0 (J2), which is what the probe reads. USB CDC is deliberately
# off: the board's own USB port is not the channel the harness can address by
# role, and enumerating it would also perturb the very bandwidth we measure.
pico_enable_stdio_uart(hwtest-xip 1)
pico_enable_stdio_usb(hwtest-xip 0)

pico_add_extra_outputs(hwtest-xip)
