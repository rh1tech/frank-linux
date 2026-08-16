/*
 * Protea - terminal compatibility layer for the ported Iris engine
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Bridges the terminal.c engine to FRANK: its config_get_*, keyboard_*,
 * serial_* and sound calls land on fixed settings, the USB HID layer and the
 * link. Declares the KEY_* / CFG_* constants terminal.c uses.
 *
 * Taken from Protea; only the implementation behind it differs.
 */

#ifndef TERM_COMPAT_H
#define TERM_COMPAT_H

#include <stdint.h>
#include <stdbool.h>

/* --- terminal type constants (Iris CFG_TTYPE_*) --- */
#define CFG_TTYPE_VT102   0
#define CFG_TTYPE_VT52    1
#define CFG_TTYPE_PETSCII 2

/* HID usage codes referenced by the engine's special-key handling. Protea's
 * input bridge does not pass raw HID codes, so these are set out of the
 * bridge's key range (0x00..0x97) to keep those branches inert. */
#define HID_KEY_PAUSE 0xF0
#define HID_KEY_F10   0xF1
#define HID_KEY_Z     0xF2

/* --- key codes expected by terminal.c (Iris keyboard.h) --- */
#define KEY_BACKSPACE   0x08
#define KEY_TAB         0x09
#define KEY_ENTER       0x0d
#define KEY_ESC         0x1b
#define KEY_DELETE      0x7f

#define KEY_UP     0x80
#define KEY_DOWN   0x81
#define KEY_LEFT   0x82
#define KEY_RIGHT  0x83
#define KEY_INSERT 0x84
#define KEY_HOME   0x85
#define KEY_END    0x86
#define KEY_PUP    0x87
#define KEY_PDOWN  0x88
#define KEY_PAUSE  0x89
#define KEY_PRSCRN 0x8a
#define KEY_F1     0x8c
#define KEY_F2     0x8d
#define KEY_F3     0x8e
#define KEY_F4     0x8f
#define KEY_F5     0x90
#define KEY_F6     0x91
#define KEY_F7     0x92
#define KEY_F8     0x93
#define KEY_F9     0x94
#define KEY_F10    0x95
#define KEY_F11    0x96
#define KEY_F12    0x97

/* --- config getters (read g_settings) --- */
uint8_t  config_get_terminal_type(void);
uint8_t  config_get_terminal_localecho(void);
uint8_t  config_get_terminal_cursortype(void);
uint8_t  config_get_terminal_cr(void);
uint8_t  config_get_terminal_lf(void);
uint8_t  config_get_terminal_bs(void);
uint8_t  config_get_terminal_del(void);
bool     config_get_terminal_clearBit7(void);
bool     config_get_terminal_uppercase(void);
uint16_t config_get_terminal_scrolldelay(void);
uint8_t  config_get_terminal_default_fg(void);
uint8_t  config_get_terminal_default_bg(void);
uint8_t  config_get_terminal_default_attr(void);
const char *config_get_terminal_answerback(void);

uint8_t  config_get_keyboard_enter(void);
uint8_t  config_get_keyboard_backspace(void);
uint8_t  config_get_keyboard_delete(void);

uint32_t config_get_serial_baud(void);

uint16_t config_get_audible_bell_frequency(void);
uint16_t config_get_audible_bell_volume(void);
uint16_t config_get_audible_bell_duration(void);
uint16_t config_get_visual_bell_color(void);
uint8_t  config_get_visual_bell_duration(void);

/* --- keyboard modifier / mapping helpers --- */
bool    keyboard_shift_pressed(uint16_t key);
bool    keyboard_ctrl_pressed(uint16_t key);
bool    keyboard_alt_pressed(uint16_t key);
bool    keyboard_russian_mode(void);
uint8_t keyboard_map_key_ascii(uint16_t key, bool *isAltCode);

/* --- serial (UART0 RS232 path) --- */
void serial_send_char(char c);
void serial_send_string(const char *s);
void serial_set_break(bool set);

/* --- font graphics-char mapping (VT100 line drawing -> CP437) --- */
uint8_t font_map_graphics_char(uint8_t c, bool boldFont);

/* --- sound (non-blocking beep) --- */
void sound_play_tone(uint16_t frequency, uint16_t duration_ms,
                     uint16_t volume, bool wait);

/* Set the current keyboard modifier state (called by the input bridge
 * before terminal_process_key so keyboard_*_pressed() report correctly). */
void term_compat_set_mods(bool shift, bool ctrl, bool alt);

/* No-op here; kept so the engine's call site still compiles. */
void serial_apply_settings(void);

/* term_compat_take_keys() lives in keyq.h: this header's KEY_* macros collide
 * with input.h's KEY_* enum, so callers that need both cannot include this. */

#endif // TERM_COMPAT_H
