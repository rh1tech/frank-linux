# Vendored fonts

Copied verbatim from the [Protea](https://github.com/rh1tech) terminal firmware
(`firmware/rp2350/src/`), by Mikhail Matveev, GPL-3.0-or-later. `font.c`'s
conversion is the authority on the sheet layout; `tools/fontgen.py` reimplements
it in Python.

They are vendored rather than referenced for two reasons. The master I/O server
and the HDMI text decoder must agree exactly about what a glyph looks like, and
they only do so if both read the same bytes. And `tools/test_screen.py` — the
decoder's only test — needs them on a machine with no Protea checkout, which is
every CI runner.

| File | Sheet | Cell |
|---|---|---|
| `font_vga.h` | 512x64, 64 bytes/row | 8x16 — what Protea renders with |
| `font_ega.h` | 512x56, 64 bytes/row | 8x14 |
| `font_cga.h` | 256x64, 32 bytes/row | 8x8 |

The CGA sheet is **half the width of the other two**, which its byte count alone
does not reveal: 2048 bytes fits 512x32 and 256x64 equally well, and the wrong
choice yields a font that looks populated but is interleaved nonsense. That is
why `fontgen.py` detects geometry by glyph shape instead of assuming it.

Set `PROTEA_SRC` to read from a Protea checkout instead, to check these against
a newer upstream sheet.
