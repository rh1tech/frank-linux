/*
 * term_compat.c - the seam between the terminal engine and FRANK.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * terminal.c is the VersaTerm-derived engine, taken from Protea unmodified. It
 * reaches the outside world through exactly two calls -- terminal_receive_char()
 * for bytes arriving from the host, and serial_send_char() for keystrokes going
 * back -- plus a set of config getters. Protea points those at a UART and a
 * settings menu. Here they point at the link, because the host is the other
 * RP2350 and there is no menu.
 *
 * That is the whole reason this port is small: the terminal does not know it is
 * talking to a kernel on another chip, and Linux does not know its console is
 * being rendered by a second processor into an HDMI signal.
 *
 * The config getters return fixed values rather than reading a settings store:
 * a Linux console wants VT102, no local echo, and each control character given
 * its plain meaning. The kernel's termios has already done any translation it
 * wants, so this side adds none.
 */

#include <stdbool.h>
#include <stdint.h>

#include "keyq.h"
#include "term_compat.h"

/* Bytes typed on the USB keyboard, on their way to Linux. Drained by the link
 * service and handed over in the next transaction. */
#define KEYQ_SIZE 256
static uint8_t keyq[KEYQ_SIZE];
static volatile uint16_t keyq_head, keyq_tail;

static bool mod_shift, mod_ctrl, mod_alt;

/* ---------- terminal behaviour ---------- */

uint8_t  config_get_terminal_type(void)       { return CFG_TTYPE_VT102; }
uint8_t  config_get_terminal_localecho(void)  { return 0; }
uint8_t  config_get_terminal_cursortype(void) { return 1; }   /* block, blinking */

/*
 * What each control character does on receipt. The engine's encoding is
 * 0 = ignore, 1 = carriage return, 2 = line feed, 3 = both -- so zero is not
 * "leave it alone", it is "throw it away", which silently joins every line of
 * output into one. Each of these is the plain meaning of the character; the
 * kernel's termios has already done any translation it wants, and a second
 * layer here would only corrupt what the shell sent.
 */
uint8_t  config_get_terminal_cr(void)         { return 1; }   /* CR: column 0 */
uint8_t  config_get_terminal_lf(void)         { return 2; }   /* LF: next row */

/* 0 = ignore, 1 = move left, 2 = move left and erase. The shell erases with
 * "\b \b" of its own, so backspace must only move. */
uint8_t  config_get_terminal_bs(void)         { return 1; }
uint8_t  config_get_terminal_del(void)        { return 0; }

bool     config_get_terminal_clearBit7(void)  { return false; }
bool     config_get_terminal_uppercase(void)  { return false; }
uint16_t config_get_terminal_scrolldelay(void) { return 0; }

uint8_t  config_get_terminal_default_fg(void)   { return 7; }   /* light grey */
uint8_t  config_get_terminal_default_bg(void)   { return 0; }   /* black */
uint8_t  config_get_terminal_default_attr(void) { return 0; }

const char *config_get_terminal_answerback(void) { return "FRANK"; }

/* The kernel sends \r for Enter and \177 for Backspace by default. */
uint8_t config_get_keyboard_enter(void)     { return 0; }
uint8_t config_get_keyboard_backspace(void) { return 0; }
uint8_t config_get_keyboard_delete(void)    { return 0; }

uint32_t config_get_serial_baud(void) { return 115200; }

/* No speaker on this path yet; the I2S DAC is Phase 7. A visual bell still
 * works because that is handled inside framebuf. */
uint16_t config_get_audible_bell_frequency(void) { return 0; }
uint16_t config_get_audible_bell_volume(void)    { return 0; }
uint16_t config_get_audible_bell_duration(void)  { return 0; }
uint16_t config_get_visual_bell_color(void)      { return 7; }
uint8_t  config_get_visual_bell_duration(void)   { return 2; }

/* ---------- keyboard ---------- */

void term_compat_set_mods(bool shift, bool ctrl, bool alt)
{
    mod_shift = shift;
    mod_ctrl  = ctrl;
    mod_alt   = alt;
}

bool keyboard_shift_pressed(uint16_t key) { (void)key; return mod_shift; }
bool keyboard_ctrl_pressed(uint16_t key)  { (void)key; return mod_ctrl; }
bool keyboard_alt_pressed(uint16_t key)   { (void)key; return mod_alt; }
bool keyboard_russian_mode(void)          { return false; }

uint8_t keyboard_map_key_ascii(uint16_t key, bool *isAltCode)
{
    if (isAltCode)
        *isAltCode = false;
    /* The HID layer already produces ASCII for printable keys; anything at or
     * above 0x80 is one of the KEY_* specials the engine handles itself. */
    return key < 0x80 ? (uint8_t)key : 0;
}

/* ---------- the host, which is Linux on the other chip ---------- */

void serial_send_char(char c)
{
    uint16_t next = (uint16_t)((keyq_head + 1) % KEYQ_SIZE);

    /* Drop rather than block: this runs from the terminal engine, and stalling
     * here would stall the display. A full queue means Linux is not draining,
     * which losing one keystroke does not make worse. */
    if (next == keyq_tail)
        return;
    keyq[keyq_head] = (uint8_t)c;
    keyq_head = next;
}

void serial_send_string(const char *s)
{
    while (*s)
        serial_send_char(*s++);
}

void serial_set_break(bool set) { (void)set; }

void serial_apply_settings(void) { }

uint32_t term_compat_take_keys(uint8_t *dst, uint32_t max)
{
    uint32_t n = 0;

    while (n < max && keyq_tail != keyq_head) {
        dst[n++] = keyq[keyq_tail];
        keyq_tail = (uint16_t)((keyq_tail + 1) % KEYQ_SIZE);
    }
    return n;
}

/* ---------- VT100 line drawing -> CP437 ---------- */

uint8_t font_map_graphics_char(uint8_t c, bool boldFont)
{
    (void)boldFont;
    static const uint8_t map[31] = {
        0x04, 0xB1, 0x0B, 0x0C, 0x0D, 0x0E, 0xF8, 0xF1, 0x0F, 0x10,
        0xD9, 0xBF, 0xDA, 0xC0, 0xC5, 0x11, 0x12, 0xC4, 0x13, 0x5F,
        0xC3, 0xB4, 0xC1, 0xC2, 0xB3, 0xF3, 0xF2, 0xE3, 0x1C, 0x9C, 0xFA
    };
    if (c >= 96 && c <= 126)
        return map[c - 96];
    return c;
}

void sound_play_tone(uint16_t frequency, uint16_t duration_ms,
                     uint16_t volume, bool wait)
{
    (void)frequency; (void)duration_ms; (void)volume; (void)wait;
}
