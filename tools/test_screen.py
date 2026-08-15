#!/usr/bin/env python3
"""
test_screen.py - round-trip test for the HDMI text decoder.

Renders a synthetic 640x480 gray frame from a Protea font, decodes it with
screen.py, and requires the text back exactly. No hardware involved, so this
catches decoder regressions independently of whether the bench is plugged in --
and it is the only way to test the decoder before the firmware that draws those
glyphs exists.

Run: python3 tools/test_screen.py
"""

from __future__ import annotations

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import fontgen          # noqa: E402
import screen           # noqa: E402

W, H = screen.WIDTH, screen.HEIGHT

# Deliberately includes the characters most likely to be confused with each
# other at 8x16 -- O/0, I/l/1, and the box-drawing block -- plus trailing and
# leading spaces, which exercise the blank-cell path.
SAMPLE = [
    "BusyBox v1.37.0 (2026-08-16 12:00:00 UTC) built-in shell (ash)",
    "",
    "  # cat /proc/cpuinfo",
    "  processor       : 0",
    "  model name      : ARMv7-M rev 0 (v7m)",
    "  I1lO0 |[]{}()<>/\\ @#$%^&*_-+=~",
    "",
    "frank2 login: root",
]


def render(text: list[str], font: fontgen.Font, cols: int, rows: int,
           scale_x: int = 1, scale_y: int = 1,
           fg: int = 0xE0, bg: int = 0x10) -> bytes:
    cell_w, cell_h = font.width * scale_x, font.height * scale_y
    x_off = (W - cols * cell_w) // 2
    y_off = (H - rows * cell_h) // 2

    buf = bytearray([bg]) * (W * H)
    for r in range(min(rows, len(text))):
        line = text[r]
        for c in range(min(cols, len(line))):
            glyph = font.rows(ord(line[c]) & 0xFF)
            for gy in range(font.height):
                bits = glyph[gy]
                if not bits:
                    continue
                for sy in range(scale_y):
                    y = y_off + r * cell_h + gy * scale_y + sy
                    base = y * W + x_off + c * cell_w
                    for gx in range(font.width):
                        if bits & (0x80 >> gx):
                            for sx in range(scale_x):
                                buf[base + gx * scale_x + sx] = fg
    return bytes(buf)


def check(name: str, got: list[str], want: list[str], confidence: float) -> bool:
    want_trimmed = [w.rstrip() for w in want]
    got_trimmed = [g.rstrip() for g in got[:len(want)]]
    if got_trimmed == want_trimmed and confidence >= 0.999:
        print(f"ok    {name}: {len(want)} lines exact, confidence {confidence:.3f}")
        return True
    print(f"FAIL  {name}: confidence {confidence:.3f}")
    for i, (g, w) in enumerate(zip(got_trimmed, want_trimmed)):
        if g != w:
            print(f"        line {i}\n          want {w!r}\n          got  {g!r}")
    return False


def main() -> int:
    ok = True

    # 1:1, the layout Protea actually renders: 80x25 of 8x16, letterboxed.
    font = fontgen.load("vga")
    raw = render(SAMPLE, font, 80, 25)
    dec = screen.Decoder(font, 8, 16)
    text, conf = dec.decode(raw, 80, 25, (W - 80 * 8) // 2, (H - 25 * 16) // 2)
    ok &= check("vga 80x25 1:1", text, SAMPLE, conf)

    # 2x scaling, to prove the integer-scale path works and is not silently
    # sampling the wrong pixel of each doubled cell.
    raw = render(SAMPLE, font, 40, 15, scale_x=2, scale_y=2)
    dec = screen.Decoder(font, 16, 32)
    text, conf = dec.decode(raw, 40, 15, (W - 40 * 16) // 2, (H - 15 * 32) // 2)
    ok &= check("vga 40x15 2x", text, [s[:40] for s in SAMPLE], conf)

    # Inverted polarity: light background, dark text. The global threshold is
    # a midpoint, so this must not decode -- it should come back as the
    # complement rather than silently producing wrong-but-confident text.
    raw = render(SAMPLE, font, 80, 25, fg=0x10, bg=0xE0)
    dec = screen.Decoder(font, 8, 16)
    text, conf = dec.decode(raw, 80, 25, (W - 80 * 8) // 2, (H - 25 * 16) // 2)
    if "BusyBox" in "\n".join(text):
        print("FAIL  inverse video decoded as if it were normal polarity")
        ok = False
    else:
        print("ok    inverse video does not masquerade as a correct decode")

    # CGA 8x8, to confirm the auto-detected 32-byte/row geometry survives a
    # real round trip and not just the shape heuristic.
    cga = fontgen.load("cga")
    raw = render(SAMPLE, cga, 80, 30)
    dec = screen.Decoder(cga, 8, 8)
    text, conf = dec.decode(raw, 80, 30, (W - 80 * 8) // 2, (H - 30 * 8) // 2)
    ok &= check("cga 80x30 1:1", text, SAMPLE, conf)

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
