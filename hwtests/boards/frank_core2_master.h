// -----------------------------------------------------
// NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER SO
//       SHOULD ONLY CONSIST OF PREPROCESSOR DIRECTIVES
// -----------------------------------------------------
//
// FRANK Core 2 Proto - master half (U3, RP2350B, QFN-80).
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Our own copy rather than a reference into frank-lab: the hardware tests are
// the evidence the whole ARM port rests on, and they should still build and
// mean the same thing if that tree moves or changes.
//
// Pin values are from frank_core2_proto/firmware/common/frank_core2_proto_board.h,
// which was extracted from the KiCad netlist.

#ifndef _BOARDS_FRANK_CORE2_MASTER_H
#define _BOARDS_FRANK_CORE2_MASTER_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

// RP2350B: 48 GPIOs. The stock pico2 definition declares PICO_RP2350A 1, which
// caps NUM_BANK0_GPIOS at 30 -- the PSRAM chip select on GPIO47 would silently
// alias down into the 0..29 range.
#define PICO_RP2350A 0

pico_board_cmake_set_default(PICO_PIO_USE_GPIO_BASE, 1)
#ifndef PICO_PIO_USE_GPIO_BASE
#define PICO_PIO_USE_GPIO_BASE 1
#endif

// UART0 on the J2 header: pin 1 = GPIO0 (TX), pin 2 = GND, pin 3 = GPIO1 (RX).
// This is the console the probe reads, so every test reports through it.
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 0
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 1
#endif

// 8 MB PSRAM (U2, ESP-PSRAM64H) chip select.
#define FRANK_PSRAM_CS_PIN 47

// U1, W25Q128JVPIQ.
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1
#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif
pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (16 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

pico_board_cmake_set_default(PICO_RP2350_A2_SUPPORTED, 1)
#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

#endif
