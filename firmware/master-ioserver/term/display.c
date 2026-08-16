/*
 * Protea - HSTX HDMI display (640x480p60, 80x25 256-colour text)
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Single video mode: 640x480p60 over DVI/HSTX, driven by one custom
 * 256-colour text slot (textmode_render.c). The 25 text rows are
 * letterboxed into the centre 400 lines.
 */

#include "display.h"
#include "disphstx.h"
#include <stdio.h>

#include "cellbuf.h"
#include "font.h"
#include "palette.h"
#include "textmode_render.h"

void display_init(void)
{
    palette_init();
    font_init();
    cellbuf_init();

    /* Install our 256-colour text format as the CUSTOM descriptor. */
    textmode_register_custom_format();

    sDispHstxVModeState *vmode = &DispHstxVMode;

    /* 640x480p60, fast variant => system clock 252 MHz. */
    DispHstxVModeInitTime(vmode, &DispHstxVModeTimeList[vmodetime_640x480_fast]);

    /* One full-height strip (480 lines). */
    DispHstxVModeAddStrip(vmode, -1);

    /* One slot: full width, custom 256-colour text format. */
    int err = DispHstxVModeAddSlot(
        vmode,
        1,                        /* hdbl: full horizontal resolution */
        1,                        /* vdbl: no line doubling           */
        640,                      /* w: 80 cols * 8 px                */
        DISPHSTX_FORMAT_CUSTOM,   /* our 256-colour text renderer     */
        cellbuf,                  /* cell buffer (source, not a fb)   */
        CELL_PITCH,               /* 320 bytes per text row           */
        palette_rgb565,           /* 256-entry RGB565 palette         */
        NULL,                     /* palvga: VGA disabled             */
        font_scanline,            /* scanline-major font              */
        FONT_HEIGHT,              /* 16 px per cell                   */
        0,                        /* no colour separator              */
        0);

    if (err != DISPHSTX_ERR_OK)
        printf("DispHSTX AddSlot error: %d\n", err);

    DispHstxSelDispMode(DISPHSTX_DISPMODE_DVI, vmode);
}

void display_wait_vsync(void)
{
    DispHstxWaitVSync();
}

void display_set_cursor(int col, int row, bool visible)
{
    textmode_set_cursor(col, row, visible);
}
