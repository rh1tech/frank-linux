/*
 * Protea - custom DispHSTX text format (256-colour, on-the-fly)
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A DISPHSTX_FORMAT_CUSTOM renderer modelled on DispHstxDviRender_ATEXT, but:
 *   - cells are 4 bytes {char, fg, bg, attr} instead of {char, 4+4-bit attr}
 *   - fg/bg are 8-bit indices into a 256-entry RGB565 palette
 *   - the 25 text rows are letterboxed (40 black lines top and bottom).
 *
 * No pixel framebuffer: each scanline is expanded directly from the cell
 * buffer into the HSTX line buffer.
 */

#include "disphstx.h"
#include "pico/platform.h"

#include "textmode_render.h"
#include "cellbuf.h"
#include "font.h"
#include "palette.h"

/* ---- cursor / blink state (read by the IRQ-time renderer) ---- */
static volatile uint8_t cur_col = 0, cur_row = 0;
static volatile bool    cur_visible = false;
static volatile uint8_t cur_beg = 14, cur_end = 15;   /* underline cursor */

void textmode_set_cursor(int col, int row, bool visible)
{
    cur_col = (uint8_t)col;
    cur_row = (uint8_t)row;
    cur_visible = visible;
}

void textmode_set_cursor_lines(uint8_t begin_line, uint8_t end_line)
{
    cur_beg = begin_line;
    cur_end = end_line;
}

/* ---- scanline renderer (must live in RAM for IRQ timing) ---- */
static void __not_in_flash_func(render_ctext256)(sDispHstxVSlot *slot, int line, u32 *cmd)
{
    u32 *dst = (u32 *)*cmd;     /* destination pixel buffer (RGB565, 2 px/u32) */

    /* Letterbox: blank the top/bottom borders.
     *
     * The destination is written through a volatile pointer so the compiler
     * cannot lower this loop to a call to the libc memset(), which lives in
     * flash. Core 1 runs this renderer at IRQ time and must stay 100% in RAM:
     * if it branched into flash while Core 0 is mid erase/program (XIP off),
     * it would hard-fault and the HSTX signal would be lost permanently. */
    int y = line - TEXT_YOFFSET;
    if ((unsigned)y >= (unsigned)TEXT_ACTIVE_H) {
        volatile u32 *bd = dst;
        for (int i = 0; i < TEXT_COLS * 4; i++) bd[i] = 0;   /* 640 px black */
        return;
    }

    int row     = y >> 4;          /* / FONT_HEIGHT (16) */
    int subline = y & 15;          /* % FONT_HEIGHT      */

    const uint8_t  *src     = (const uint8_t *)slot->buf + row * slot->pitch;
    const uint8_t  *fontrow = (const uint8_t *)slot->font + subline * 256;
    const uint16_t *pal     = (const uint16_t *)slot->pal;

    /* blink / cursor phase (~1 Hz): bit 19 of the microsecond timer */
    uint32_t t = time_us_32();
    bool phase_on  = (t & (1u << 19)) != 0;
    bool cursor_on = cur_visible && phase_on && (row == cur_row);

    for (int col = 0; col < TEXT_COLS; col++) {
        uint8_t ch   = src[CELL_CHAR];
        uint8_t fgi  = src[CELL_FG];
        uint8_t bgi  = src[CELL_BG];
        uint8_t attr = src[CELL_ATTR];
        src += CELL_BYTES;

        uint32_t fg = pal[fgi];
        uint32_t bg = pal[bgi];
        if (attr & ATTR_INVERSE) { uint32_t tmp = fg; fg = bg; bg = tmp; }

        uint8_t g = fontrow[ch];
        if ((attr & ATTR_UNDERLINE) && subline == FONT_UNDERLINE) g = 0xFF;
        if ((attr & ATTR_BLINK) && !phase_on)                     g = 0x00;
        if (cursor_on && col == cur_col && subline >= cur_beg && subline <= cur_end)
            g = (uint8_t)~g;

        /* 2-pixel colour pairs: index = {bit_left, bit_right} */
        uint32_t map2[4];
        map2[0] = bg | (bg << 16);
        map2[1] = bg | (fg << 16);
        map2[2] = fg | (bg << 16);
        map2[3] = fg | (fg << 16);

        dst[0] = map2[g >> 6];
        dst[1] = map2[(g >> 4) & 3];
        dst[2] = map2[(g >> 2) & 3];
        dst[3] = map2[g & 3];
        dst += 4;
    }
}

/* ---- format descriptor (mirrors ATEXT, palnum=256, RGB565 output) ---- */
static const sDispHstxVColor ctext256_vcolor = {
    .textmode  = 1,
    .colmode   = 0,
    .tilemode  = 0,
    .hstxmode  = 0,
    .userender = 1,
    .attrmode  = 0,
    .graphmode = 0,

    .colbits   = 16,
    .grp       = DISPHSTX_GRP_16,
    .tilew     = 0,
    .palnum    = 256,

    .defpal    = NULL,
    .defpalvga = NULL,

    /* RGB565 TMDS expander setup (identical to built-in RGB565 formats) */
    .expand_tmds = (4u << 21) | (8u << 16) | (5u << 13) | (3u << 8) | (4u << 5) | (29u << 0),

    .render_dvi = render_ctext256,
    .render_vga = NULL,
};

void textmode_register_custom_format(void)
{
    DispHstxVColorCustom = &ctext256_vcolor;
}
