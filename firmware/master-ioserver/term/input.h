/*
 * Protea - unified keyboard input layer
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Turns the USB HID key-action stream into a single event flow that
 * carries both printable ASCII (shift-aware) and named special keys
 * (arrows, Enter, Esc, F-keys, navigation). Built on
 * usbhid_get_key_action() so the menu and the console share one source.
 */

#ifndef INPUT_H
#define INPUT_H

#include <stdbool.h>

/* Special keys are >= 0x100 so they don't collide with ASCII. */
enum {
    KEY_NONE = 0,

    KEY_ENTER = 0x100,
    KEY_ESC,
    KEY_BACKSPACE,
    KEY_TAB,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_PGUP,
    KEY_PGDN,
    KEY_HOME,
    KEY_END,
    KEY_DELETE,
    KEY_INSERT,

    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12
};

/* A key-down event: either a printable ASCII char (0x20..0x7E) or one of
 * the KEY_* special codes. */
typedef struct {
    int  code;        /* ASCII char or KEY_* */
    bool shift;
    bool ctrl;
    bool alt;
} input_event_t;

void input_init(void);

/* Pop the next key-down event. Returns false when none pending. */
bool input_poll(input_event_t *ev);

#endif // INPUT_H
