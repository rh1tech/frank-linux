#!/usr/bin/env python3
"""
screen.py - decode the HDMI capture back into text.

The capture card offers native 640x480, which is exactly the HSTX output mode,
so frames arrive pixel-exact with no rescale. Because the font and the grid
geometry are both known, a frame decodes deterministically into a character
matrix: match each cell against the font sheet. Screen assertions are then
string comparisons, not image diffs and not OCR.

Frames are pulled as raw gray8 rather than PNG so there is no image-library
dependency: 640*480 bytes, one per pixel, straight off ffmpeg's stdout.

Usage:
    screen.py                          decode and print the screen
    screen.py --expect 'BusyBox'       assert, exit 1 if absent
    screen.py --png logs/screen.png    also save the frame
    screen.py --from-raw f.gray        decode a saved frame instead of capturing
    screen.py --grid 53x30 --cell 12x16 --font cga     other layouts
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fontgen  # noqa: E402

WIDTH, HEIGHT = 640, 480
FRAME = WIDTH * HEIGHT


def bench(key: str, default: str) -> str:
    conf = os.environ.get("FRANK_BENCH_CONF") or os.path.join(HERE, "bench.conf")
    if os.path.exists(conf):
        with open(conf) as fh:
            for raw in fh:
                line = raw.split("#", 1)[0].strip()
                if line.startswith(key + "="):
                    return line.split("=", 1)[1].strip()
    return default


def grab(frames: int = 4) -> bytes:
    """
    Capture and return the last frame as gray8.

    Several frames are requested and only the last is kept: the MS2109 needs a
    moment to lock, and its first frame after opening is routinely a torn or
    stale one. Asserting on that frame produces failures that vanish on retry,
    which is worse than no test at all.
    """
    device = bench("CAPTURE_VIDEO_DEVICE", "USB Video")
    cmd = [
        "ffmpeg", "-hide_banner", "-loglevel", "error",
        "-f", "avfoundation",
        "-pixel_format", "uyvy422",
        "-video_size", f"{WIDTH}x{HEIGHT}",
        "-framerate", "60",
        "-i", device,
        "-frames:v", str(frames),
        "-pix_fmt", "gray", "-f", "rawvideo", "-",
    ]
    out = subprocess.run(cmd, capture_output=True)
    if len(out.stdout) < FRAME:
        sys.exit(f"capture failed ({len(out.stdout)} bytes)\n"
                 f"{out.stderr.decode(errors='replace').strip()}")
    return out.stdout[-FRAME:]


def save_png(raw: bytes, path: str) -> None:
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error",
         "-f", "rawvideo", "-pix_fmt", "gray",
         "-video_size", f"{WIDTH}x{HEIGHT}", "-i", "-", "-y", path],
        input=raw, check=True)


class Decoder:
    def __init__(self, font: fontgen.Font, cell_w: int, cell_h: int):
        self.font = font
        self.cell_w = cell_w
        self.cell_h = cell_h
        # Integer scale from font cell to screen cell. Protea renders 8x16 at
        # 1:1; the bring-up firmware renders its 6x8 font at 2x. Anything that
        # is not an integer multiple is a layout we do not understand, and
        # guessing would produce confident wrong text.
        self.sx = cell_w // font.width
        self.sy = cell_h // font.height
        if self.sx * font.width != cell_w or self.sy * font.height != cell_h:
            sys.exit(f"cell {cell_w}x{cell_h} is not an integer multiple of the "
                     f"{font.width}x{font.height} font")

        # Exact-match table first. The capture is pixel-exact, so almost every
        # cell hits here and the expensive nearest-neighbour search never runs.
        self.exact: dict[tuple[int, ...], int] = {}
        for code in range(256):
            key = tuple(font.rows(code))
            self.exact.setdefault(key, code)

    def cell_bits(self, raw: bytes, x0: int, y0: int, thresh: int) -> tuple[int, ...]:
        """One screen cell as font-resolution bitmask rows."""
        rows = []
        for gy in range(self.font.height):
            y = y0 + gy * self.sy
            base = y * WIDTH + x0
            bits = 0
            for gx in range(self.font.width):
                if raw[base + gx * self.sx] > thresh:
                    bits |= 0x80 >> gx
            rows.append(bits)
        return tuple(rows)

    def match(self, key: tuple[int, ...]) -> tuple[int, float]:
        hit = self.exact.get(key)
        if hit is not None:
            return hit, 1.0

        total = self.font.height * self.font.width
        best, best_score = ord("?"), -1
        for code in range(256):
            same = 0
            for a, b in zip(key, self.font.rows(code)):
                same += 8 - bin(a ^ b).count("1")
            if same > best_score:
                best, best_score = code, same
        return best, best_score / total

    def decode(self, raw: bytes, cols: int, rows: int,
               x_off: int, y_off: int) -> tuple[list[str], float]:
        # One global threshold, midway between the darkest and brightest pixel
        # in the frame. Per-cell thresholds sound more adaptive but turn a blank
        # cell's sensor noise into arbitrary glyphs, because a uniform cell has
        # no meaningful min/max to split.
        lo, hi = min(raw), max(raw)
        thresh = (lo + hi) // 2

        # Confidence is averaged over inked cells only. A text screen is mostly
        # blank, and blank cells always match perfectly, so including them
        # drowns the signal: decoding a 6x8-at-2x screen with an 8x16 font --
        # entirely the wrong font -- still scored 0.901 that way, comfortably
        # inside any plausible floor. Over inked cells the same decode is
        # obviously bad.
        text, confidences = [], []
        for r in range(rows):
            line = []
            for c in range(cols):
                key = self.cell_bits(raw, x_off + c * self.cell_w,
                                     y_off + r * self.cell_h, thresh)
                if not any(key):
                    line.append(" ")
                    continue
                code, conf = self.match(key)
                confidences.append(conf)
                line.append(chr(code) if 32 <= code < 127 else
                            ("█" if code == 0xDB else "·"))
            text.append("".join(line).rstrip())
        # An entirely blank screen is not evidence of a good decode, but it is
        # not evidence of a bad one either; report it as perfect and let the
        # --expect patterns fail on the missing content.
        return text, (sum(confidences) / len(confidences) if confidences else 1.0)


def parse_pair(s: str, what: str) -> tuple[int, int]:
    m = re.fullmatch(r"(\d+)x(\d+)", s)
    if not m:
        sys.exit(f"bad {what}: {s!r} (expected NxM)")
    return int(m.group(1)), int(m.group(2))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--font", default="vga")
    ap.add_argument("--grid", default="80x25", help="columns x rows")
    ap.add_argument("--cell", default="8x16", help="cell width x height in pixels")
    ap.add_argument("--origin", default="", help="X,Y of the grid; default centres it")
    ap.add_argument("--expect", action="append", default=[],
                    help="regex that must appear; repeatable")
    ap.add_argument("--png")
    ap.add_argument("--from-raw")
    ap.add_argument("--save-raw")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--min-confidence", type=float, default=0.98,
                    help="warn below this mean per-cell match (default 0.98)")
    args = ap.parse_args()

    cols, rows = parse_pair(args.grid, "grid")
    cell_w, cell_h = parse_pair(args.cell, "cell")

    if args.origin:
        x_off, y_off = (int(v) for v in args.origin.split(","))
    else:
        # Centre the grid. Protea letterboxes 80x25 of 8x16 in 640x480, leaving
        # 40 blank lines top and bottom.
        x_off = (WIDTH - cols * cell_w) // 2
        y_off = (HEIGHT - rows * cell_h) // 2
    if x_off < 0 or y_off < 0 or \
       x_off + cols * cell_w > WIDTH or y_off + rows * cell_h > HEIGHT:
        sys.exit(f"grid {cols}x{rows} of {cell_w}x{cell_h} does not fit in "
                 f"{WIDTH}x{HEIGHT} at origin {x_off},{y_off}")

    raw = open(args.from_raw, "rb").read()[-FRAME:] if args.from_raw else grab()
    if args.save_raw:
        open(args.save_raw, "wb").write(raw)
    if args.png:
        save_png(raw, args.png)

    font = fontgen.load(args.font)
    text, confidence = Decoder(font, cell_w, cell_h).decode(
        raw, cols, rows, x_off, y_off)

    # A screen with nothing on it decodes as rows of spaces, which prints as a
    # blank block and reads exactly like the tool having failed. Say which it
    # is: a blank screen is a real result about the machine, not about ffmpeg.
    if not any(line.strip() for line in text):
        print("(screen is blank -- capture worked, there is nothing on it)",
              file=sys.stderr)

    if not args.quiet:
        for line in text:
            print(line)
        print(f"\n[{font.name} {font.width}x{font.height}, grid {cols}x{rows} "
              f"@ {x_off},{y_off}, mean confidence {confidence:.3f}]",
              file=sys.stderr)

    # Never silent about a poor decode, even under --quiet. A pixel-exact
    # capture of the right font at the right geometry scores essentially 1.000;
    # anything much lower means the font, the cell size or the origin is wrong,
    # and the characters above are guesses. An assertion that passes on a
    # guessed screen is worse than one that fails.
    if confidence < args.min_confidence:
        print(f"WARN  screen: mean confidence {confidence:.3f} < "
              f"{args.min_confidence:.3f} -- decoded text is unreliable. "
              f"Check --font/--cell/--grid/--origin.", file=sys.stderr)

    joined = "\n".join(text)
    failed = [p for p in args.expect if not re.search(p, joined)]
    for p in failed:
        print(f"FAIL  screen: /{p}/ not found", file=sys.stderr)
    if args.expect and not failed:
        if confidence < args.min_confidence:
            print("FAIL  screen: patterns matched but the decode is "
                  "below the confidence floor -- not treating that as a pass",
                  file=sys.stderr)
            return 1
        print(f"PASS  screen: all {len(args.expect)} pattern(s) matched",
              file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
