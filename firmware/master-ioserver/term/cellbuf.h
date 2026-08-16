/*
 * Protea - 80x25 character-cell buffer
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * No pixel framebuffer: text lives only as cells, rendered on the fly per
 * scanline by textmode_render.c.  Each cell is 4 bytes {char, fg, bg, attr}.
 */

#ifndef CELLBUF_H
#define CELLBUF_H

#include <stdint.h>

#define TEXT_COLS   80
#define TEXT_ROWS   25
#define CELL_BYTES  4
#define CELL_PITCH  (TEXT_COLS * CELL_BYTES)          /* 320 bytes / row */

/* Cell byte offsets within a cell. */
#define CELL_CHAR   0
#define CELL_FG     1
#define CELL_BG     2
#define CELL_ATTR   3

/* Attribute bits (match iris terminal semantics). */
#define ATTR_UNDERLINE 0x01
#define ATTR_BLINK     0x02
#define ATTR_BOLD      0x04
#define ATTR_INVERSE   0x08

/* Cell buffer, 32-bit aligned (required by DispHSTX slot buffer). */
extern uint8_t cellbuf[TEXT_ROWS * CELL_PITCH] __attribute__((aligned(4)));

void cellbuf_init(void);

/* Set / get a single cell. */
void cellbuf_put(int col, int row, uint8_t ch, uint8_t fg, uint8_t bg, uint8_t attr);
void cellbuf_set_char(int col, int row, uint8_t ch);
void cellbuf_set_color(int col, int row, uint8_t fg, uint8_t bg);
void cellbuf_set_attr(int col, int row, uint8_t attr);

/* Fill region / whole screen with a character and colour. */
void cellbuf_fill(int col0, int row0, int col1, int row1,
                  uint8_t ch, uint8_t fg, uint8_t bg, uint8_t attr);
void cellbuf_clear(uint8_t fg, uint8_t bg);

/* Write a NUL-terminated string starting at (col,row); no wrapping. */
void cellbuf_puts(int col, int row, const char *s, uint8_t fg, uint8_t bg, uint8_t attr);

/* Scroll the whole screen up (n>0) or down (n<0) by n rows; vacated rows
 * filled with space using fg/bg. */
void cellbuf_scroll(int n, uint8_t fg, uint8_t bg);

#endif // CELLBUF_H
