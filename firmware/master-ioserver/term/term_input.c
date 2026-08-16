/*
 * Protea - keyboard -> terminal input bridge
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Maps Protea input events (ASCII + KEY_* specials) onto the key codes the
 * ported terminal engine expects (Iris encoding: specials 0x80+, control
 * characters for Ctrl combos), then calls terminal_process_key().
 */

#include "term_input.h"
#include "terminal.h"

/* Iris key encoding (kept local to avoid clashing with input.h's KEY_*). */
enum {
    IK_UP = 0x80, IK_DOWN, IK_LEFT, IK_RIGHT, IK_INSERT, IK_HOME, IK_END,
    IK_PUP, IK_PDOWN,
    IK_F1 = 0x8c, IK_F2, IK_F3, IK_F4, IK_F5, IK_F6, IK_F7, IK_F8, IK_F9,
    IK_F10, IK_F11, IK_F12
};

extern void term_compat_set_mods(bool shift, bool ctrl, bool alt);

static uint16_t map_special(int code)
{
    switch (code) {
        case KEY_ENTER:     return 0x0d;
        case KEY_ESC:       return 0x1b;
        case KEY_BACKSPACE: return 0x08;
        case KEY_TAB:       return 0x09;
        case KEY_DELETE:    return 0x7f;
        case KEY_UP:        return IK_UP;
        case KEY_DOWN:      return IK_DOWN;
        case KEY_LEFT:      return IK_LEFT;
        case KEY_RIGHT:     return IK_RIGHT;
        case KEY_INSERT:    return IK_INSERT;
        case KEY_HOME:      return IK_HOME;
        case KEY_END:       return IK_END;
        case KEY_PGUP:      return IK_PUP;
        case KEY_PGDN:      return IK_PDOWN;
        case KEY_F1:        return IK_F1;
        case KEY_F2:        return IK_F2;
        case KEY_F3:        return IK_F3;
        case KEY_F4:        return IK_F4;
        case KEY_F5:        return IK_F5;
        case KEY_F6:        return IK_F6;
        case KEY_F7:        return IK_F7;
        case KEY_F8:        return IK_F8;
        case KEY_F9:        return IK_F9;
        case KEY_F10:       return IK_F10;
        case KEY_F11:       return IK_F11;
        case KEY_F12:       return IK_F12;
        default:            return 0;
    }
}

/* Apply Ctrl to a printable character, producing a control code. */
static uint16_t apply_ctrl(int c)
{
    if (c >= 'a' && c <= 'z') return (uint16_t)(c - 'a' + 1);   /* ^A..^Z */
    if (c >= 'A' && c <= 'Z') return (uint16_t)(c - 'A' + 1);
    if (c >= '[' && c <= '_') return (uint16_t)(c - '@');       /* ^[ ^\ ^] ^^ ^_ */
    if (c == '@' || c == ' ') return 0;                         /* ^@ / ^space = NUL */
    if (c == '?')             return 0x7f;
    return (uint16_t)c;
}

void terminal_feed_event(const input_event_t *ev)
{
    term_compat_set_mods(ev->shift, ev->ctrl, ev->alt);

    uint16_t key;
    if (ev->code >= 0x100) {
        key = map_special(ev->code);
        if (key == 0) return;
    } else if (ev->ctrl) {
        key = apply_ctrl(ev->code);
    } else {
        key = (uint16_t)ev->code;
    }

    terminal_process_key(key);
}
