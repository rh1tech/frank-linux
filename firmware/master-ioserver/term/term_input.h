/*
 * Protea - keyboard -> terminal input bridge
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TERM_INPUT_H
#define TERM_INPUT_H

#include "input.h"

/* Translate a Protea input event into an Iris key code (applying Ctrl) and
 * pass it to terminal_process_key, after updating modifier state. */
void terminal_feed_event(const input_event_t *ev);

#endif // TERM_INPUT_H
