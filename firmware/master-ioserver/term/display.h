/*
 * Protea - HSTX HDMI display (640x480p60, 80x25 256-colour text)
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdbool.h>

/* Bring up the HSTX DVI video mode and the 80x25 text renderer.
 * Initialises palette, font and cell buffer. */
void display_init(void);

/* Block until vertical sync. */
void display_wait_vsync(void);

/* Position / show / hide the text cursor. */
void display_set_cursor(int col, int row, bool visible);

/* Per-frame housekeeping that does not block. See src/display_tick.c. */
void display_tick(void);

#endif // DISPLAY_H
