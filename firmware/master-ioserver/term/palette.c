/*
 * Protea - 256-colour palette (xterm-256 -> RGB565)
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "palette.h"

uint16_t palette_rgb565[256];

/* 16 standard ANSI system colours (RGB888), VGA/xterm style. */
static const uint32_t sys16[16] = {
    0x000000, 0x800000, 0x008000, 0x808000,
    0x000080, 0x800080, 0x008080, 0xC0C0C0,
    0x808080, 0xFF0000, 0x00FF00, 0xFFFF00,
    0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF
};

static inline uint16_t rgb888_to_rgb565(uint32_t rgb888)
{
    uint8_t r = (uint8_t)(rgb888 >> 16);
    uint8_t g = (uint8_t)(rgb888 >> 8);
    uint8_t b = (uint8_t)(rgb888);
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3));
}

void palette_set_rgb888(uint8_t index, uint32_t rgb888)
{
    palette_rgb565[index] = rgb888_to_rgb565(rgb888);
}

void palette_init(void)
{
    /* 0..15: system colours */
    for (int i = 0; i < 16; i++)
        palette_rgb565[i] = rgb888_to_rgb565(sys16[i]);

    /* 16..231: 6x6x6 colour cube */
    static const uint8_t lvl[6] = { 0, 95, 135, 175, 215, 255 };
    int idx = 16;
    for (int r = 0; r < 6; r++)
        for (int g = 0; g < 6; g++)
            for (int b = 0; b < 6; b++)
                palette_rgb565[idx++] =
                    rgb888_to_rgb565(((uint32_t)lvl[r] << 16) |
                                     ((uint32_t)lvl[g] << 8)  |
                                      (uint32_t)lvl[b]);

    /* 232..255: 24-step greyscale ramp (8,18,...,238) */
    for (int i = 0; i < 24; i++) {
        uint8_t v = (uint8_t)(8 + i * 10);
        palette_rgb565[232 + i] =
            rgb888_to_rgb565(((uint32_t)v << 16) | ((uint32_t)v << 8) | v);
    }
}
