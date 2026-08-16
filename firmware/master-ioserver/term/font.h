/*
 * Protea - Font system
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * Font data reused from iris-2350 (VGA 8x16 bitmap sheet).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef FONT_H
#define FONT_H

#include <stdint.h>

#define FONT_WIDTH       8
#define FONT_HEIGHT      16     /* 8x16 VGA cell => 25 rows in 400 px */
#define FONT_UNDERLINE   14     /* underline scanline within the cell */

/*
 * Renderer-ready font: scanline-major, stride 256.
 *   glyph row byte = font_scanline[subline * 256 + char]
 *   bit 7 (MSB) = leftmost pixel (matches DispHSTX RGB565 output order).
 * Size = FONT_HEIGHT * 256 = 4096 bytes, kept in RAM for fast IRQ access.
 */
extern uint8_t font_scanline[FONT_HEIGHT * 256];

/* Build font_scanline from the embedded VGA bitmap sheet. */
void font_init(void);

#endif // FONT_H
