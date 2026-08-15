// -----------------------------------------------------
// NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER SO
//       SHOULD ONLY CONSIST OF PREPROCESSOR DIRECTIVES
// -----------------------------------------------------
//
// FRANK Core 2 Proto - slave half (U6, RP2350A, QFN-60). Runs Linux..
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Our own copy rather than a reference into frank-lab: the hardware tests are
// the evidence the whole ARM port rests on, and they should still build and
// mean the same thing if that tree moves or changes.
//
// Pin values are from frank_core2_proto/firmware/common/frank_core2_proto_board.h,
// which was extracted from the KiCad netlist.

#ifndef _BOARDS_FRANK_CORE2_SLAVE_H
#define _BOARDS_FRANK_CORE2_SLAVE_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

// RP2350A, QFN-60: the stock 30-GPIO layout applies.
#define PICO_RP2350A 1

pico_board_cmake_set_default(PICO_PIO_USE_GPIO_BASE, 1)
#ifndef PICO_PIO_USE_GPIO_BASE
#define PICO_PIO_USE_GPIO_BASE 1
#endif

// UART1 on the J4 header (GPIO24 TX / GPIO25 RX).
//
// Not UART0: on this half GPIO0 is the PSRAM chip select, so the SDK's default
// UART0 placement would drive the chip select as a serial line.
//
// J4 is not wired to anything on this bench, which is the whole reason the
// console moves to USB CDC served by core 1 -- Linux cannot drive the RP2350's
// USB controller itself. Output here is harmless and occasionally useful with a
// dongle attached.
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 1
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 24
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 25
#endif

// Blue status LED (LD2) via 1K R19, active high.
#ifndef PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_LED_PIN 26
#endif

// 8 MB PSRAM (U5, ESP-PSRAM64H) chip select.
#define FRANK_PSRAM_CS_PIN 0

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
