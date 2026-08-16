/*
 * Protea - custom DispHSTX text format (256-colour, on-the-fly)
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TEXTMODE_RENDER_H
#define TEXTMODE_RENDER_H

#include <stdint.h>
#include <stdbool.h>

/* Vertical letterbox: 25 rows * 16 px = 400 active lines centred in 480. */
#define TEXT_YOFFSET   40
#define TEXT_ACTIVE_H  400

/* Install our descriptor as DispHstxVColorCustom. Call before AddSlot. */
void textmode_register_custom_format(void);

/* Cursor control (consumed by the scanline renderer). */
void textmode_set_cursor(int col, int row, bool visible);
void textmode_set_cursor_lines(uint8_t begin_line, uint8_t end_line);

#endif // TEXTMODE_RENDER_H
