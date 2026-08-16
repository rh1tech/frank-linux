/*
 * storage.h - the medium behind /dev/frankblk0 on the other half.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef STORAGE_H
#define STORAGE_H

#include <stdint.h>

#define STORAGE_SECTOR 512u

/* Returns the sector count and, through `medium`, a name for what answered.
 * A zero return means nothing did. */
uint32_t storage_init(const char **medium);

int32_t storage_read(uint32_t lba, uint32_t count, void *buf);
int32_t storage_write(uint32_t lba, uint32_t count, const void *buf);

#endif /* STORAGE_H */
