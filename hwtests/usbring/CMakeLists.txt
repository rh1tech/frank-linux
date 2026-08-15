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

include(${HWTESTS_ROOT}/pico_sdk_import.cmake)
project(hwtest-usbring C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(hwtest-usbring
    src/main.c
    ${HWTESTS_ROOT}/../firmware/common/core1_usb.c
    ${HWTESTS_ROOT}/../firmware/common/usb_descriptors.c
)

target_include_directories(hwtest-usbring PRIVATE
    src
    ${HWTESTS_ROOT}/common
    ${HWTESTS_ROOT}/../firmware/common
)

target_compile_definitions(hwtest-usbring PRIVATE
    CPU_SPEED=${CPU_SPEED}
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
)

# -O1, not -O2 or -Os. The exclusive sequences must keep their LDREX and STREX
# in one basic block: anything the compiler inserts between them -- a spill, a
# call, a reordered load -- is architecturally allowed to clear the local
# monitor, which would make a perfectly good region look like it never grants
# an exclusive. -O1 keeps the intrinsics adjacent while still being real code.
target_compile_options(hwtest-usbring PRIVATE -O1 -Wall -Wextra -Wno-unused-parameter)

target_link_libraries(hwtest-usbring
    pico_stdlib
    hardware_clocks
    hardware_gpio
    hardware_sync
    hardware_timer
    hardware_vreg
    hardware_xip_cache
    pico_multicore
    tinyusb_device
    tinyusb_board
)

# Console on UART0 (J2), which is what the probe reads. USB CDC is deliberately
# off: the board's own USB port is not the channel the harness can address by
# role, and enumerating it would also perturb the very bandwidth we measure.
pico_enable_stdio_uart(hwtest-usbring 1)
pico_enable_stdio_usb(hwtest-usbring 0)

pico_add_extra_outputs(hwtest-usbring)
