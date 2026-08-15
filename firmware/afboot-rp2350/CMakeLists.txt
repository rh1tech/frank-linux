#
# afboot-rp2350 - bootloader for the slave half.
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# SPDX-License-Identifier: GPL-3.0-or-later
#
cmake_minimum_required(VERSION 3.13)

set(FW_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(HWTESTS_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../hwtests)

set(PICO_BOARD_HEADER_DIRS ${HWTESTS_ROOT}/boards CACHE PATH "" FORCE)
# Linux runs on the slave, so this only ever builds for that half.
set(PICO_BOARD "frank_core2_slave" CACHE STRING "Pico board type" FORCE)

set(CPU_SPEED   "252" CACHE STRING "CPU clock in MHz")
set(PSRAM_SPEED "133" CACHE STRING "PSRAM max frequency in MHz")
set(FLASH_SPEED "66"  CACHE STRING "Flash max frequency in MHz")

include(${HWTESTS_ROOT}/pico_sdk_import.cmake)
project(afboot-rp2350 C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(afboot-rp2350
    src/main.c
    ${FW_ROOT}/common/core1_usb.c
    ${FW_ROOT}/common/usb_descriptors.c
    ${HWTESTS_ROOT}/common/psram_init.c
)

target_include_directories(afboot-rp2350 PRIVATE
    src
    ${FW_ROOT}/common
    ${HWTESTS_ROOT}/common
)

target_compile_definitions(afboot-rp2350 PRIVATE
    CPU_SPEED=${CPU_SPEED}
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
    FLASH_MAX_FREQ_MHZ=${FLASH_SPEED}
)

target_compile_options(afboot-rp2350 PRIVATE -Os -Wall -Wextra -Wno-unused-parameter)

target_link_libraries(afboot-rp2350
    pico_stdlib
    pico_multicore
    hardware_clocks
    hardware_gpio
    hardware_sync
    hardware_vreg
    hardware_xip_cache
    tinyusb_device
    tinyusb_board
)

# No stdio at all. The console is the SRAM ring served by core 1, which is what
# the kernel will use too -- a UART here would stop working at handover, and J4
# is not wired on this bench in any case.
pico_enable_stdio_uart(afboot-rp2350 0)
pico_enable_stdio_usb(afboot-rp2350 0)

pico_add_extra_outputs(afboot-rp2350)
