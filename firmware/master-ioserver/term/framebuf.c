/*
 * Protea - framebuf compatibility shim over the 80x25 cell buffer
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "framebuf.h"
#include "cellbuf.h"
#include <string.h>

#define NCOLS TEXT_COLS
#define NROWS TEXT_ROWS

static uint8_t row_attr[NROWS];
static bool    screen_inverted = false;

/* visual-bell flash state */
static uint8_t  flash_frames = 0;
static uint8_t  flash_color  = 0;

static inline uint8_t *cell(int col, int row)
{
    return &cellbuf[row * CELL_PITCH + col * CELL_BYTES];
}

static inline bool in_range(int col, int row)
{
    return col >= 0 && col < NCOLS && row >= 0 && row < NROWS;
}

void framebuf_init(void)
{
    memset(row_attr, 0, sizeof(row_attr));
    screen_inverted = false;
    flash_frames = 0;
    cellbuf_clear(7, 0);
}

void framebuf_set_char(uint8_t column, uint8_t row, uint8_t character)
{
    if (in_range(column, row)) cell(column, row)[CELL_CHAR] = character;
}

uint8_t framebuf_get_char(uint8_t column, uint8_t row)
{
    return in_range(column, row) ? cell(column, row)[CELL_CHAR] : ' ';
}

void framebuf_set_attr(uint8_t column, uint8_t row, uint8_t a)
{
    if (in_range(column, row)) cell(column, row)[CELL_ATTR] = a;
}

uint8_t framebuf_get_attr(uint8_t column, uint8_t row)
{
    return in_range(column, row) ? cell(column, row)[CELL_ATTR] : 0;
}

void framebuf_set_row_attr(uint8_t row, uint8_t a)
{
    if (row < NROWS) row_attr[row] = a;
}

uint8_t framebuf_get_row_attr(uint8_t row)
{
    return row < NROWS ? row_attr[row] : 0;
}

void framebuf_set_color(uint8_t column, uint8_t row, uint8_t fg, uint8_t bg)
{
    if (in_range(column, row)) {
        uint8_t *c = cell(column, row);
        c[CELL_FG] = fg;
        c[CELL_BG] = bg;
    }
}

void framebuf_fill_region(uint8_t cs, uint8_t rs, uint8_t ce, uint8_t re,
                          char ch, uint8_t fg, uint8_t bg)
{
    for (int r = rs; r <= re && r < NROWS; r++)
        for (int c = cs; c <= ce && c < NCOLS; c++) {
            uint8_t *p = cell(c, r);
            p[CELL_CHAR] = (uint8_t)ch;
            p[CELL_FG]   = fg;
            p[CELL_BG]   = bg;
            p[CELL_ATTR] = 0;
        }
}

void framebuf_fill_screen(char ch, uint8_t fg, uint8_t bg)
{
    memset(row_attr, 0, sizeof(row_attr));
    framebuf_fill_region(0, 0, NCOLS - 1, NROWS - 1, ch, fg, bg);
}

/* Scroll rows [row_start..row_end] by n (n>0 up, n<0 down); vacated rows
 * filled with spaces using fg/bg. */
void framebuf_scroll_region(uint8_t row_start, uint8_t row_end, int8_t n,
                            uint8_t fg, uint8_t bg)
{
    if (row_end >= NROWS) row_end = NROWS - 1;
    if (row_start > row_end) return;
    int rows = row_end - row_start + 1;
    int cnt = n < 0 ? -n : n;
    if (cnt > rows) cnt = rows;

    if (n > 0) {                       /* scroll up */
        for (int r = row_start; r + cnt <= row_end; r++)
            memcpy(cell(0, r), cell(0, r + cnt), CELL_PITCH);
        framebuf_fill_region(0, row_end - cnt + 1, NCOLS - 1, row_end, ' ', fg, bg);
    } else if (n < 0) {                /* scroll down */
        for (int r = row_end; r - cnt >= row_start; r--)
            memcpy(cell(0, r), cell(0, r - cnt), CELL_PITCH);
        framebuf_fill_region(0, row_start, NCOLS - 1, row_start + cnt - 1, ' ', fg, bg);
    }
}

/* Insert n blank cells at (x,y), shifting the rest of the row right. */
void framebuf_insert(uint8_t x, uint8_t y, uint8_t n, uint8_t fg, uint8_t bg)
{
    if (y >= NROWS || x >= NCOLS) return;
    if (n > NCOLS - x) n = NCOLS - x;
    for (int c = NCOLS - 1; c >= x + n; c--)
        memcpy(cell(c, y), cell(c - n, y), CELL_BYTES);
    framebuf_fill_region(x, y, x + n - 1, y, ' ', fg, bg);
}

/* Delete n cells at (x,y), shifting the rest of the row left. */
void framebuf_delete(uint8_t x, uint8_t y, uint8_t n, uint8_t fg, uint8_t bg)
{
    if (y >= NROWS || x >= NCOLS) return;
    if (n > NCOLS - x) n = NCOLS - x;
    for (int c = x; c + n < NCOLS; c++)
        memcpy(cell(c, y), cell(c + n, y), CELL_BYTES);
    framebuf_fill_region(NCOLS - n, y, NCOLS - 1, y, ' ', fg, bg);
}

uint8_t framebuf_get_nrows(void) { return NROWS; }
uint8_t framebuf_get_ncols(int row) { (void)row; return NCOLS; }

void framebuf_set_scroll_delay(uint16_t ms) { (void)ms; }  /* instant scroll */

void framebuf_set_screen_inverted(bool invert)
{
    if (invert == screen_inverted) return;
    screen_inverted = invert;
    for (int r = 0; r < NROWS; r++)
        for (int c = 0; c < NCOLS; c++)
            cell(c, r)[CELL_ATTR] ^= ATTR_INVERSE;
}

void framebuf_flash_screen(uint8_t color, uint8_t nframes)
{
    if (nframes == 0) return;
    flash_color  = color;
    flash_frames = nframes;
    for (int r = 0; r < NROWS; r++)
        for (int c = 0; c < NCOLS; c++)
            cell(c, r)[CELL_ATTR] ^= ATTR_INVERSE;
}

void framebuf_task(void)
{
    if (flash_frames > 0 && --flash_frames == 0) {
        for (int r = 0; r < NROWS; r++)
            for (int c = 0; c < NCOLS; c++)
                cell(c, r)[CELL_ATTR] ^= ATTR_INVERSE;
    }
}
