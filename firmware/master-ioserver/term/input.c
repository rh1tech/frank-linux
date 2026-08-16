/*
 * Protea - unified keyboard input layer
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "input.h"
#include "usbhid.h"
#include <stdint.h>

/* HID modifier bit positions (tinyusb layout). */
#define MOD_LSHIFT 0x02
#define MOD_RSHIFT 0x20
#define MOD_LCTRL  0x01
#define MOD_RCTRL  0x10
#define MOD_LALT   0x04
#define MOD_RALT   0x40

static bool shift_down = false;
static bool ctrl_down  = false;
static bool alt_down   = false;

void input_init(void)
{
    shift_down = ctrl_down = alt_down = false;
}

/* Map a HID usage keycode to a KEY_* special, or 0 if not special. */
static int keycode_to_special(uint8_t kc)
{
    switch (kc) {
        case 0x28: return KEY_ENTER;
        case 0x58: return KEY_ENTER;     /* keypad enter */
        case 0x29: return KEY_ESC;
        case 0x2A: return KEY_BACKSPACE;
        case 0x2B: return KEY_TAB;
        case 0x4F: return KEY_RIGHT;
        case 0x50: return KEY_LEFT;
        case 0x51: return KEY_DOWN;
        case 0x52: return KEY_UP;
        case 0x4B: return KEY_PGUP;
        case 0x4E: return KEY_PGDN;
        case 0x4A: return KEY_HOME;
        case 0x4D: return KEY_END;
        case 0x4C: return KEY_DELETE;
        case 0x49: return KEY_INSERT;
        case 0x3A: return KEY_F1;
        case 0x3B: return KEY_F2;
        case 0x3C: return KEY_F3;
        case 0x3D: return KEY_F4;
        case 0x3E: return KEY_F5;
        case 0x3F: return KEY_F6;
        case 0x40: return KEY_F7;
        case 0x41: return KEY_F8;
        case 0x42: return KEY_F9;
        case 0x43: return KEY_F10;
        case 0x44: return KEY_F11;
        case 0x45: return KEY_F12;
        default:   return 0;
    }
}

bool input_poll(input_event_t *ev)
{
    uint8_t kc;
    int down;

    while (usbhid_get_key_action(&kc, &down)) {
        /* Track modifier state (emitted as synthetic keycodes by hid_app). */
        if (kc == 0xE1) { shift_down = down; continue; }
        if (kc == 0xE0) { ctrl_down  = down; continue; }
        if (kc == 0xE2) { alt_down   = down; continue; }

        if (!down) continue;   /* only act on key-down */

        int special = keycode_to_special(kc);
        int code;
        if (special) {
            code = special;
        } else {
            uint8_t mod = shift_down ? (MOD_LSHIFT) : 0;
            uint8_t ch = usbhid_keycode_to_ascii(kc, mod);
            if (!ch || ch < 0x20 || ch > 0x7E) continue;  /* skip non-printable */
            code = ch;
        }

        ev->code  = code;
        ev->shift = shift_down;
        ev->ctrl  = ctrl_down;
        ev->alt   = alt_down;
        return true;
    }
    return false;
}
