#
# Phase 2 hardware test: RP2350 exclusive monitor behaviour outside SRAM.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
cmake_minimum_required(VERSION 3.13)

set(HWTESTS_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

set(PICO_BOARD_HEADER_DIRS ${HWTESTS_ROOT}/boards CACHE PATH "" FORCE)
set(PICO_BOARD "frank_core2_master" CACHE STRING "Pico board type" FORCE)

set(CPU_SPEED   "252" CACHE STRING "CPU clock in MHz")
set(PSRAM_SPEED "133" CACHE STRING "PSRAM max frequency in MHz")

include(${HWTESTS_ROOT}/pico_sdk_import.cmake)
project(hwtest-exclusives C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(hwtest-exclusives
    src/main.c
    ${HWTESTS_ROOT}/common/psram_init.c
)

target_include_directories(hwtest-exclusives PRIVATE
    src
    ${HWTESTS_ROOT}/common
)

target_compile_definitions(hwtest-exclusives PRIVATE
    CPU_SPEED=${CPU_SPEED}
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
)

# -O1, not -O2 or -Os. The exclusive sequences must keep their LDREX and STREX
# in one basic block: anything the compiler inserts between them -- a spill, a
# call, a reordered load -- is architecturally allowed to clear the local
# monitor, which would make a perfectly good region look like it never grants
# an exclusive. -O1 keeps the intrinsics adjacent while still being real code.
target_compile_options(hwtest-exclusives PRIVATE -O1 -Wall -Wextra -Wno-unused-parameter)

target_link_libraries(hwtest-exclusives
    pico_stdlib
    hardware_clocks
    hardware_gpio
    hardware_sync
    hardware_timer
    hardware_vreg
    hardware_xip_cache
)

# Console on UART0 (J2), which is what the probe reads. USB CDC is deliberately
# off: the board's own USB port is not the channel the harness can address by
# role, and enumerating it would also perturb the very bandwidth we measure.
pico_enable_stdio_uart(hwtest-exclusives 1)
pico_enable_stdio_usb(hwtest-exclusives 0)

pico_add_extra_outputs(hwtest-exclusives)
