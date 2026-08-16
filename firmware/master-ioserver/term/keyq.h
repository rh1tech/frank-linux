/*
 * keyq.h - keystrokes waiting to go to Linux.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A header of its own rather than a line in term_compat.h, because that header
 * defines KEY_* as macros and input.h defines KEY_* as an enum. Including both
 * in one translation unit does not compile -- which is also why Protea keeps
 * the terminal engine and the input layer in separate files.
 */

#ifndef KEYQ_H
#define KEYQ_H

#include <stdint.h>

/* Take up to `max` queued keystrokes for the next link transaction. Returns
 * how many were written. */
uint32_t term_compat_take_keys(uint8_t *dst, uint32_t max);

#endif /* KEYQ_H */
