/*
 * Protea - Font system
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Converts the iris-2350 VGA bitmap sheet (512x64 px, bottom-up, LSB-first)
 * into a renderer-ready scanline-major buffer (stride 256, MSB-first).
 * Conversion mirrors iris set_font_data().
 */

#include "font.h"
#include "pico.h"        /* __in_flash() used by font_vga.h */
#include "font_vga.h"   /* font_vga_bmp[4096] : 512x64 px sheet, ch=16 */

uint8_t font_scanline[FONT_HEIGHT * 256];

/* VGA sheet geometry (from iris font_get_font_info: VGA) */
#define SHEET_W  512   /* pixels wide  -> 64 chars across (512/8) */
#define SHEET_H  64    /* pixels tall  -> 4 char-rows (64/16)     */

void font_init(void)
{
    const int bytes_per_row = SHEET_W / 8;   /* 64 char-columns */

    for (int br = 0; br < SHEET_H; br++)
    {
        for (int bc = 0; bc < bytes_per_row; bc++)
        {
            /* iris mapping: sheet is stored bottom-up */
            int cr = (SHEET_H - br - 1) % FONT_HEIGHT;                 /* 0..15  */
            int cn = ((SHEET_H - br - 1) / FONT_HEIGHT) * bytes_per_row + bc; /* 0..255 */

            uint8_t d = font_vga_bmp[br * bytes_per_row + bc];
            /* Source font is already MSB=leftmost, which is what our
             * ATEXT-style renderer expects (bit7 -> first pixel). */

            font_scanline[cr * 256 + cn] = d;
        }
    }
}
