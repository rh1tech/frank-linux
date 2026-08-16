/*
 * DispHSTX configuration.
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Force-included before DispHSTX sees its own _config.h, and named config.h
 * because the library's .S files include it by that name -- renaming it makes
 * the assembler fail with a missing header rather than a wrong setting.
 *
 * From Protea, unchanged: the FRANK master's HDMI pin map is byte-identical to
 * Protea's, both CLK-=12 through D2+=19, so PINOUT 2 is already correct.
 * We drive the display with a single CUSTOM 256-colour text format, so all
 * built-in pixel/text formats are disabled to save flash/RAM.
 */

#ifndef DISPHSTX_CONFIG_H
#define DISPHSTX_CONFIG_H

#define DISPHSTX_PICOSDK    1       // Use PicoSDK (not PicoLibSDK)
#define USE_DISPHSTX        1       // Enable HSTX display driver

#define USE_DRAWCAN         0       // We use our own (custom) renderer
#define USE_DRAWCAN0        0
#define USE_DRAWCAN1        0
#define USE_DRAWCAN2        0
#define USE_DRAWCAN3        0
#define USE_DRAWCAN4        0
#define USE_DRAWCAN6        0
#define USE_DRAWCAN8        0
#define USE_DRAWCAN12       0
#define USE_DRAWCAN16       0
#define USE_RAND            0
#define USE_TEXT            0

// DVI (HDMI) only, no VGA
#define DISPHSTX_USE_DVI    1
#define DISPHSTX_USE_VGA    0

// FRANK master J5 (and Protea M2): CLK-=12 .. D2+=19
#define DISPHSTX_DVI_PINOUT 2       // order CLK-..D2+

// All built-in formats disabled; rendering is done by our DISPHSTX_FORMAT_CUSTOM
// descriptor (see textmode_render.c).
#define DISPHSTX_USE_FORMAT_1           0
#define DISPHSTX_USE_FORMAT_2           0
#define DISPHSTX_USE_FORMAT_3           0
#define DISPHSTX_USE_FORMAT_4           0
#define DISPHSTX_USE_FORMAT_6           0
#define DISPHSTX_USE_FORMAT_8           0
#define DISPHSTX_USE_FORMAT_12          0
#define DISPHSTX_USE_FORMAT_15          0
#define DISPHSTX_USE_FORMAT_16          0
#define DISPHSTX_USE_FORMAT_1_PAL       0
#define DISPHSTX_USE_FORMAT_2_PAL       0
#define DISPHSTX_USE_FORMAT_3_PAL       0
#define DISPHSTX_USE_FORMAT_4_PAL       0
#define DISPHSTX_USE_FORMAT_6_PAL       0
#define DISPHSTX_USE_FORMAT_8_PAL       0
#define DISPHSTX_USE_FORMAT_COL         0
#define DISPHSTX_USE_FORMAT_MTEXT       0
#define DISPHSTX_USE_FORMAT_ATEXT       0
#define DISPHSTX_USE_FORMAT_TILE4_8     0
#define DISPHSTX_USE_FORMAT_TILE8_8     0
#define DISPHSTX_USE_FORMAT_TILE16_8    0
#define DISPHSTX_USE_FORMAT_TILE32_8    0
#define DISPHSTX_USE_FORMAT_TILE4_8_PAL 0
#define DISPHSTX_USE_FORMAT_TILE8_8_PAL 0
#define DISPHSTX_USE_FORMAT_TILE16_8_PAL 0
#define DISPHSTX_USE_FORMAT_TILE32_8_PAL 0
#define DISPHSTX_USE_FORMAT_HSTX_15    0
#define DISPHSTX_USE_FORMAT_HSTX_16    0
#define DISPHSTX_USE_FORMAT_PAT_8      0
#define DISPHSTX_USE_FORMAT_PAT_8_PAL  0
#define DISPHSTX_USE_FORMAT_RLE8       0
#define DISPHSTX_USE_FORMAT_RLE8_PAL   0
#define DISPHSTX_USE_FORMAT_ATTR1_PAL  0
#define DISPHSTX_USE_FORMAT_ATTR2_PAL  0
#define DISPHSTX_USE_FORMAT_ATTR3_PAL  0
#define DISPHSTX_USE_FORMAT_ATTR4_PAL  0
#define DISPHSTX_USE_FORMAT_ATTR5_PAL  0
#define DISPHSTX_USE_FORMAT_ATTR6_PAL  0
#define DISPHSTX_USE_FORMAT_ATTR7_PAL  0
#define DISPHSTX_USE_FORMAT_ATTR8_PAL  0

// Limit max width to save RAM on render line buffers
#define DISPHSTX_WIDTHMAX   640

// One strip, one slot
#define DISPHSTX_STRIP_MAX  1
#define DISPHSTX_SLOT_MAX   1

#endif // DISPHSTX_CONFIG_H
