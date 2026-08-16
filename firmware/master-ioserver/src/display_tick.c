/*
 * display_tick.c - per-frame terminal housekeeping, off the critical path.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Protea's main loop ends in display_wait_vsync(), which paces it at 60 Hz.
 * That is right for a terminal and wrong here: this half also serves the other
 * chip's disk, and blocking 16 ms per iteration would cap it at one transaction
 * per frame. So the cursor and the visual bell are serviced on a frame *edge*
 * detected without waiting, and the loop stays free to answer the link.
 */

#include "pico/stdlib.h"

#include "display.h"
#include "framebuf.h"
#include "terminal.h"

void display_tick(void)
{
    static uint32_t next_us;
    uint32_t now = time_us_32();

    /* ~60 Hz, but never blocking. Wraps cleanly because the difference is
     * computed in unsigned arithmetic. */
    if ((int32_t)(now - next_us) < 0)
        return;
    next_us = now + 16667u;

    framebuf_task();

    int col, row;
    bool visible;
    terminal_get_cursor(&col, &row, &visible);
    display_set_cursor(col, row, visible);
}
