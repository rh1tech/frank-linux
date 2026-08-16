/*
 * Protea - 256-colour palette (xterm-256 -> RGB565)
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PALETTE_H
#define PALETTE_H

#include <stdint.h>

/* 256-entry RGB565 palette, kept in RAM for fast IRQ access by the renderer. */
extern uint16_t palette_rgb565[256];

/* Build the xterm-256 palette:
 *   0..15   : 16 system colours
 *   16..231 : 6x6x6 colour cube
 *   232..255: 24 greyscale ramp
 */
void palette_init(void);

/* Set one entry from RGB888 (0xRRGGBB) at runtime. */
void palette_set_rgb888(uint8_t index, uint32_t rgb888);

/* Convenience: the 16 ANSI base colour indices. */
enum {
    COL_BLACK = 0, COL_RED, COL_GREEN, COL_YELLOW,
    COL_BLUE, COL_MAGENTA, COL_CYAN, COL_WHITE,
    COL_BRIGHT_BLACK, COL_BRIGHT_RED, COL_BRIGHT_GREEN, COL_BRIGHT_YELLOW,
    COL_BRIGHT_BLUE, COL_BRIGHT_MAGENTA, COL_BRIGHT_CYAN, COL_BRIGHT_WHITE
};

#endif // PALETTE_H
