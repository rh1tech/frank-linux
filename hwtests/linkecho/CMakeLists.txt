#
# Phase 2 hardware test: RP2350 exclusive monitor behaviour outside SRAM.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
cmake_minimum_required(VERSION 3.13)

set(HWTESTS_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

set(PICO_BOARD_HEADER_DIRS ${HWTESTS_ROOT}/boards CACHE PATH "" FORCE)
# Which half. The two differ in package (48 vs 30 GPIOs), PSRAM chip select
# (GPIO47 vs GPIO0) and which UART is the console.
set(FRANK_HALF "master" CACHE STRING "master or slave")
set(PICO_BOARD "frank_core2_${FRANK_HALF}" CACHE STRING "Pico board type" FORCE)

set(CPU_SPEED   "252" CACHE STRING "CPU clock in MHz")
set(PSRAM_SPEED "133" CACHE STRING "PSRAM max frequency in MHz")
set(FLASH_SPEED "66"  CACHE STRING "Flash max frequency in MHz")

include(${HWTESTS_ROOT}/pico_sdk_import.cmake)
project(hwtest-linkecho C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(hwtest-linkecho
    src/main.c
    ${HWTESTS_ROOT}/../firmware/common/link_bus.c
    ${HWTESTS_ROOT}/../firmware/common/core1_usb.c
    ${HWTESTS_ROOT}/../firmware/common/usb_descriptors.c
)

# The PIO programs for the two link directions.
pico_generate_pio_header(hwtest-linkecho
    ${HWTESTS_ROOT}/../firmware/common/link_bus.pio)

target_include_directories(hwtest-linkecho PRIVATE
    src
    ${HWTESTS_ROOT}/common
    ${HWTESTS_ROOT}/../firmware/common
)

target_compile_definitions(hwtest-linkecho PRIVATE
    CPU_SPEED=${CPU_SPEED}
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
    FLASH_MAX_FREQ_MHZ=${FLASH_SPEED}
)

# One source builds both ends; this is the only thing that distinguishes them.
# Get it wrong and the master silently compiles the slave's receive path, waits
# for a doorbell that nobody rings, and reports nothing -- which is exactly what
# happened when this define was missing.
if(FRANK_HALF STREQUAL "master")
    target_compile_definitions(hwtest-linkecho PRIVATE FRANK_IS_MASTER=1)
endif()

# -O1, not -O2 or -Os. The exclusive sequences must keep their LDREX and STREX
# in one basic block: anything the compiler inserts between them -- a spill, a
# call, a reordered load -- is architecturally allowed to clear the local
# monitor, which would make a perfectly good region look like it never grants
# an exclusive. -O1 keeps the intrinsics adjacent while still being real code.
target_compile_options(hwtest-linkecho PRIVATE -O1 -Wall -Wextra -Wno-unused-parameter)

target_link_libraries(hwtest-linkecho
    pico_stdlib
    hardware_clocks
    hardware_gpio
    hardware_sync
    hardware_timer
    hardware_vreg
    hardware_xip_cache
    hardware_pio
    hardware_dma
    pico_multicore
    tinyusb_device
    tinyusb_board
)

# Console on UART0 (J2), which is what the probe reads. USB CDC is deliberately
# off: the board's own USB port is not the channel the harness can address by
# role, and enumerating it would also perturb the very bandwidth we measure.
# The master reports over UART0 (J2, read by its probe); the slave has no
# wired UART and reports through the core-1 USB console instead.
pico_enable_stdio_uart(hwtest-linkecho 1)
pico_enable_stdio_usb(hwtest-linkecho 0)

pico_add_extra_outputs(hwtest-linkecho)
