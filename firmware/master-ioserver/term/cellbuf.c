/*
 * Protea - 80x25 character-cell buffer
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "cellbuf.h"
#include <string.h>

uint8_t cellbuf[TEXT_ROWS * CELL_PITCH] __attribute__((aligned(4)));

static inline uint8_t *cell_ptr(int col, int row)
{
    return &cellbuf[row * CELL_PITCH + col * CELL_BYTES];
}

void cellbuf_put(int col, int row, uint8_t ch, uint8_t fg, uint8_t bg, uint8_t attr)
{
    if ((unsigned)col >= TEXT_COLS || (unsigned)row >= TEXT_ROWS) return;
    uint8_t *c = cell_ptr(col, row);
    c[CELL_CHAR] = ch;
    c[CELL_FG]   = fg;
    c[CELL_BG]   = bg;
    c[CELL_ATTR] = attr;
}

void cellbuf_set_char(int col, int row, uint8_t ch)
{
    if ((unsigned)col >= TEXT_COLS || (unsigned)row >= TEXT_ROWS) return;
    cell_ptr(col, row)[CELL_CHAR] = ch;
}

void cellbuf_set_color(int col, int row, uint8_t fg, uint8_t bg)
{
    if ((unsigned)col >= TEXT_COLS || (unsigned)row >= TEXT_ROWS) return;
    uint8_t *c = cell_ptr(col, row);
    c[CELL_FG] = fg;
    c[CELL_BG] = bg;
}

void cellbuf_set_attr(int col, int row, uint8_t attr)
{
    if ((unsigned)col >= TEXT_COLS || (unsigned)row >= TEXT_ROWS) return;
    cell_ptr(col, row)[CELL_ATTR] = attr;
}

void cellbuf_fill(int col0, int row0, int col1, int row1,
                  uint8_t ch, uint8_t fg, uint8_t bg, uint8_t attr)
{
    if (col0 < 0) col0 = 0;
    if (row0 < 0) row0 = 0;
    if (col1 >= TEXT_COLS) col1 = TEXT_COLS - 1;
    if (row1 >= TEXT_ROWS) row1 = TEXT_ROWS - 1;
    for (int r = row0; r <= row1; r++)
        for (int c = col0; c <= col1; c++)
            cellbuf_put(c, r, ch, fg, bg, attr);
}

void cellbuf_clear(uint8_t fg, uint8_t bg)
{
    cellbuf_fill(0, 0, TEXT_COLS - 1, TEXT_ROWS - 1, ' ', fg, bg, 0);
}

void cellbuf_puts(int col, int row, const char *s, uint8_t fg, uint8_t bg, uint8_t attr)
{
    while (*s && col < TEXT_COLS) {
        cellbuf_put(col++, row, (uint8_t)*s++, fg, bg, attr);
    }
}

void cellbuf_scroll(int n, uint8_t fg, uint8_t bg)
{
    if (n == 0) return;
    if (n >= TEXT_ROWS || -n >= TEXT_ROWS) {
        cellbuf_clear(fg, bg);
        return;
    }
    if (n > 0) {
        memmove(&cellbuf[0], &cellbuf[n * CELL_PITCH],
                (TEXT_ROWS - n) * CELL_PITCH);
        cellbuf_fill(0, TEXT_ROWS - n, TEXT_COLS - 1, TEXT_ROWS - 1, ' ', fg, bg, 0);
    } else {
        n = -n;
        memmove(&cellbuf[n * CELL_PITCH], &cellbuf[0],
                (TEXT_ROWS - n) * CELL_PITCH);
        cellbuf_fill(0, 0, TEXT_COLS - 1, n - 1, ' ', fg, bg, 0);
    }
}

void cellbuf_init(void)
{
    cellbuf_clear(7 /* light grey fg */, 0 /* black bg */);
}
