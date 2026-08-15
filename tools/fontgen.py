#!/usr/bin/env python3
"""
fontgen.py - load the Protea bitmap fonts for screen decoding.

The master I/O server renders text with Protea's fonts, so the HDMI decoder has
to match glyphs against those exact bitmaps. This module reads the C headers
directly rather than keeping a converted copy: a second copy of a font is a
second thing to keep in sync, and a decoder matching against a stale font
reports wrong characters rather than failing.

Sheet format, from protea/firmware/rp2350/src/font.c: glyphs are packed into a
pixel sheet stored BOTTOM-UP with MSB = leftmost pixel, 256 glyphs per font.

font.c only ever converts the VGA sheet, and its geometry is hardcoded there as
512x64. The other two headers do not share it -- measured here, VGA and EGA are
512 px wide (64 bytes per row, heights 16 and 14) but CGA is 256 px wide (32
bytes per row, height 8). Byte count alone does not disambiguate: CGA's 2048
bytes fit 512x32 and 256x64 equally well, and the wrong choice yields a font
that looks populated but is interleaved nonsense.

So geometry is detected, not assumed, and the detection is decided by glyph
shapes that only come out right in one orientation and packing.

Usage:
    fontgen.py show <font.h> <char>     render one glyph as ASCII art
    fontgen.py selftest <font.h>        check the transform against known shapes
"""

from __future__ import annotations

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

SHEET_BYTES_PER_ROW = 64          # 512 px / 8
GLYPHS_ACROSS = 64
NUM_GLYPHS = 256

FONTS = {
    "vga": "font_vga.h",   # 8x16, the one Protea actually renders with
    "ega": "font_ega.h",   # 8x14
    "cga": "font_cga.h",   # 8x8
}

# Vendored copies come first. The master I/O server links these same headers, so
# the decoder and the firmware are guaranteed to agree about what a glyph looks
# like -- and CI can run the decoder tests without the Protea checkout mounted.
# PROTEA_SRC still overrides, for checking against a newer upstream sheet.
VENDORED = os.path.join(os.path.dirname(HERE), "firmware", "fonts")
PROTEA_SRC = os.environ.get(
    "PROTEA_SRC", "/Volumes/1TB/Repositories/protea/firmware/rp2350/src")

_HEX = re.compile(rb"0x([0-9A-Fa-f]{2})")


class Font:
    """256 glyphs, each `height` rows of 8 horizontal pixels, MSB leftmost."""

    def __init__(self, glyphs: list[list[int]], height: int, name: str,
                 bytes_per_row: int = 0):
        self.glyphs = glyphs
        self.height = height
        self.width = 8
        self.name = name
        self.bytes_per_row = bytes_per_row

    def rows(self, code: int) -> list[int]:
        return self.glyphs[code]

    def art(self, code: int) -> str:
        out = []
        for row in self.glyphs[code]:
            out.append("".join("#" if row & (0x80 >> x) else "." for x in range(8)))
        return "\n".join(out)


def resolve(spec: str) -> str:
    """Accept 'vga' / 'ega' / 'cga' or a path to a header."""
    if spec not in FONTS:
        return spec
    if "PROTEA_SRC" in os.environ:
        return os.path.join(os.environ["PROTEA_SRC"], FONTS[spec])
    vendored = os.path.join(VENDORED, FONTS[spec])
    if os.path.exists(vendored):
        return vendored
    return os.path.join(PROTEA_SRC, FONTS[spec])


def _unpack(data: bytes, bytes_per_row: int, height: int) -> list[list[int]] | None:
    """Apply font.c's bottom-up transform for one candidate geometry."""
    if len(data) % bytes_per_row:
        return None
    sheet_rows = len(data) // bytes_per_row
    if sheet_rows % height:
        return None
    if (sheet_rows // height) * bytes_per_row != NUM_GLYPHS:
        return None

    glyphs = [[0] * height for _ in range(NUM_GLYPHS)]
    for br in range(sheet_rows):
        # Bottom-up: the last pixel row holds char-row 0, scanline 0.
        flipped = sheet_rows - br - 1
        cr = flipped % height
        base = (flipped // height) * bytes_per_row
        for bc in range(bytes_per_row):
            glyphs[base + bc][cr] = data[br * bytes_per_row + bc]
    return glyphs


def _ink_rows(glyph: list[int]) -> list[int]:
    return [i for i, r in enumerate(glyph) if r]


def score(glyphs: list[list[int]], height: int) -> int:
    """
    How much does this candidate geometry look like a real Latin font?

    Every check is orientation- or packing-sensitive, because a wrong guess
    still produces a full set of populated glyphs -- just interleaved or upside
    down -- and a decoder built on one reports confident nonsense. Underscore
    and apostrophe are the sharpest tests: they are single marks that live at
    opposite ends of the cell, so a vertical flip swaps them.
    """
    pts = 0

    if not any(glyphs[ord(" ")]):
        pts += 2                                    # space must be blank

    for ch in "ABCDEFGHILMNOPRSTUW0123456789":
        if any(glyphs[ord(ch)]):
            pts += 1                                # printable must have ink

    under = _ink_rows(glyphs[ord("_")])
    if under and min(under) >= height * 0.55:
        pts += 4                                    # underscore sits low

    apos = _ink_rows(glyphs[ord("'")])
    if apos and max(apos) <= height * 0.55:
        pts += 4                                    # apostrophe sits high

    ell = glyphs[ord("L")]
    ink = _ink_rows(ell)
    if ink:
        foot = bin(ell[max(ink)]).count("1")
        head = bin(ell[min(ink)]).count("1")
        if foot > head:
            pts += 4                                # L's bar is on the bottom

    # A solid block: every row full width. Survives only correct packing.
    if all(r == 0xFF for r in glyphs[0xDB]):
        pts += 3

    return pts


def load(spec: str, geometry: tuple[int, int] | None = None) -> Font:
    path = resolve(spec)
    if not os.path.exists(path):
        raise SystemExit(f"font not found: {path}\n"
                         f"expected a vendored copy in {VENDORED}; "
                         f"set PROTEA_SRC to read from a protea checkout instead")

    with open(path, "rb") as fh:
        data = bytes(int(m.group(1), 16) for m in _HEX.finditer(fh.read()))
    if not data:
        raise SystemExit(f"{path}: no hex byte literals found")

    if geometry:
        bpr, height = geometry
        glyphs = _unpack(data, bpr, height)
        if glyphs is None:
            raise SystemExit(f"{path}: {len(data)} bytes do not fit "
                             f"{bpr} bytes/row x {height} rows x 256 glyphs")
        return Font(glyphs, height, os.path.basename(path), bpr)

    best = None
    for bpr in (64, 32, 16, 128):
        for height in (16, 14, 8, 12, 10):
            glyphs = _unpack(data, bpr, height)
            if glyphs is None:
                continue
            s = score(glyphs, height)
            if best is None or s > best[0]:
                best = (s, glyphs, height, bpr)

    if best is None:
        raise SystemExit(f"{path}: no candidate geometry fits {len(data)} bytes")

    s, glyphs, height, bpr = best
    # A correct font scores in the 40s; a scrambled one lands well below.
    if s < 30:
        raise SystemExit(f"{path}: best geometry ({bpr} bytes/row, {height} rows) "
                         f"only scored {s} -- the sheet layout is not understood")
    return Font(glyphs, height, os.path.basename(path), bpr)


def selftest(font: Font) -> int:
    s = score(font.glyphs, font.height)
    ok = s >= 30
    print(("ok    " if ok else "FAIL  ") +
          f"{font.name}: 8x{font.height}, {NUM_GLYPHS} glyphs, "
          f"{font.bytes_per_row} bytes/row, shape score {s}")
    return 0 if ok else 1


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    cmd, spec = argv[1], argv[2]
    font = load(spec)

    if cmd == "selftest":
        return selftest(font)
    if cmd == "show":
        if len(argv) != 4:
            print("usage: fontgen.py show <font> <char>", file=sys.stderr)
            return 2
        ch = argv[3]
        code = int(ch, 0) if ch.startswith("0") and len(ch) > 1 else ord(ch[0])
        print(f"{font.name} glyph {code} ({chr(code)!r}), 8x{font.height}:")
        print(font.art(code))
        return 0

    print(f"unknown command: {cmd}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
