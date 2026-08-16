/*
 * storage.c - what the master actually serves over the link.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The microSD is on this half's SPI0 (J7) and the half running Linux has no
 * card slot of its own, so this is the real medium behind /dev/frankblk0.
 *
 * The card driver is vendored from the FRANK bring-up firmware, which took it
 * from ChaN's FatFs sample. Only the diskio layer is used -- disk_initialize,
 * disk_read, disk_write, disk_ioctl -- because Linux owns the filesystem. ff.c
 * is deliberately not compiled in; there is no FAT code on this side at all.
 *
 * If there is no card, the RAM disk stands in. That is a fallback, so it says
 * so on the console and reports a different medium name: an absent card must
 * not be able to masquerade as a working one, which is exactly the trap the
 * earlier "grep for a string that appears either way" test checks fell into.
 */

#include <string.h>

#include "pico/stdlib.h"

#include "ff.h"          /* must precede diskio.h: it defines BYTE, LBA_t, ... */
#include "diskio.h"
#include "storage.h"

#define RAMDISK_SECTORS  128u

static uint8_t ramdisk[RAMDISK_SECTORS * STORAGE_SECTOR];
static bool have_card;

/* Sector N filled with a pattern derived from N, so a read can be checked
 * without the two halves exchanging reference data first. */
static void ramdisk_fill(void)
{
    for (uint32_t s = 0; s < RAMDISK_SECTORS; s++) {
        uint8_t *p = ramdisk + s * STORAGE_SECTOR;
        for (uint32_t i = 0; i < STORAGE_SECTOR; i += 4) {
            uint32_t v = (s << 16) | i;
            p[i] = (uint8_t)v; p[i+1] = (uint8_t)(v >> 8);
            p[i+2] = (uint8_t)(v >> 16); p[i+3] = (uint8_t)(v >> 24);
        }
    }
}

uint32_t storage_init(const char **medium)
{
    DWORD sectors = 0;

    if (disk_initialize(0) == 0 &&
        disk_ioctl(0, GET_SECTOR_COUNT, &sectors) == RES_OK && sectors) {
        have_card = true;
        *medium = "microSD";
        return (uint32_t)sectors;
    }

    ramdisk_fill();
    have_card = false;
    *medium = "ramdisk (NO CARD)";
    return RAMDISK_SECTORS;
}

int32_t storage_read(uint32_t lba, uint32_t count, void *buf)
{
    if (have_card)
        return disk_read(0, buf, lba, count) == RES_OK ? 0 : -5;

    if (lba + count > RAMDISK_SECTORS)
        return -5;
    memcpy(buf, ramdisk + lba * STORAGE_SECTOR, count * STORAGE_SECTOR);
    return 0;
}

int32_t storage_write(uint32_t lba, uint32_t count, const void *buf)
{
    if (have_card)
        return disk_write(0, buf, lba, count) == RES_OK ? 0 : -5;

    if (lba + count > RAMDISK_SECTORS)
        return -5;
    memcpy(ramdisk + lba * STORAGE_SECTOR, buf, count * STORAGE_SECTOR);
    return 0;
}
