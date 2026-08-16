#!/usr/bin/env python3
"""
test_romfs.py - romfs.py against images built here, no hardware needed.

SPDX-License-Identifier: GPL-3.0-or-later

romfs.py exists so that questions about the flash rootfs can be answered on the
host instead of by flashing the board. That is only worth anything if the parser
is right, and "it printed a plausible tree" is not evidence: a parser that
follows the sibling chain slightly wrong produces a listing that looks fine and
is missing a subdirectory.

So the images here are built byte by byte with known contents, including the
cases that have actually gone wrong: a file whose data is not page-aligned
(genromfs's default, which silently costs execute-in-place), a symlink, and a
directory whose "." and ".." entries point back at their parents -- the shape
that makes a naive walker recurse until it runs out of stack.
"""

from __future__ import annotations

import struct
import subprocess
import sys
import tempfile
import os

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import romfs  # noqa: E402

FAILURES = []


def check(name: str, ok: bool, detail: str = "") -> None:
    if ok:
        print(f"ok    {name}")
    else:
        print(f"FAIL  {name}{': ' + detail if detail else ''}")
        FAILURES.append(name)


def align16(n: int) -> int:
    return (n + 15) & ~15


class Builder:
    """
    A minimal romfs writer.

    Deliberately not genromfs: a test that builds its input with the same tool
    the thing under test was written against checks that two programs agree,
    not that either is right. This follows the format description instead, so a
    misreading in romfs.py has to coincide with the same misreading here.
    """

    def __init__(self, volume: str = "TEST"):
        self.blob = bytearray()
        self.blob += b"-rom1fs-"
        self.blob += b"\0\0\0\0"                    # size, patched at the end
        self.blob += b"\0\0\0\0"                    # checksum, not verified
        name = volume.encode() + b"\0"
        self.blob += name + b"\0" * (align16(16 + len(name)) - 16 - len(name))

    def pad_to(self, alignment: int) -> None:
        while len(self.blob) % alignment:
            self.blob += b"\0"

    def entry(self, name: str, kind: int, spec: int, data: bytes,
              execbit: bool = False, next_off: int = 0,
              align_data: int = 16) -> int:
        """Write one header plus its data; returns this entry's offset."""
        off = len(self.blob)
        nm = name.encode() + b"\0"
        hdr_len = align16(16 + len(nm))
        # The data of a regular file may need to land on a page boundary, and
        # the only lever is padding *before* the header, since the header's
        # length is fixed by the name.
        if align_data > 16:
            pad = (-(off + hdr_len)) % align_data
            if pad:
                self.blob += b"\0" * pad
                off = len(self.blob)
        nextw = (next_off & ~0xF) | kind | (0x8 if execbit else 0)
        self.blob += struct.pack(">IIII", nextw, spec, len(data), 0)
        self.blob += nm + b"\0" * (hdr_len - 16 - len(nm))
        self.blob += data
        self.pad_to(16)
        return off

    def finish(self) -> bytes:
        struct.pack_into(">I", self.blob, 8, len(self.blob))
        return bytes(self.blob)


def build_image(page_align: bool) -> bytes:
    """
    /            dir
      .   ..     back-pointers, which a naive walker follows forever
      hello      regular file, executable
      link       symlink -> hello
      sub/       dir
        .  ..
        deep     regular file

    Offsets are patched afterwards because a header has to name the next
    sibling, which is not known until it has been written.
    """
    b = Builder()
    a = 4096 if page_align else 16

    dot = b.entry(".", 1, 0, b"")
    dotdot = b.entry("..", 1, 0, b"")
    hello = b.entry("hello", 2, 0, b"HELLO ROMFS\n", execbit=True, align_data=a)
    link = b.entry("link", 3, 0, b"hello")
    sub = b.entry("sub", 1, 0, b"")
    sdot = b.entry(".", 1, 0, b"")
    sdotdot = b.entry("..", 1, 0, b"")
    deep = b.entry("deep", 2, 0, b"DEEP\n", align_data=a)

    def link_to(off: int, nxt: int) -> None:
        cur = struct.unpack_from(">I", b.blob, off)[0]
        struct.pack_into(">I", b.blob, off, (nxt & ~0xF) | (cur & 0xF))

    def spec_to(off: int, target: int) -> None:
        struct.pack_into(">I", b.blob, off + 4, target)

    # Root chain: . -> .. -> hello -> link -> sub -> end
    link_to(dot, dotdot)
    link_to(dotdot, hello)
    link_to(hello, link)
    link_to(link, sub)
    link_to(sub, 0)
    spec_to(dot, dot)          # "." points at its own directory
    spec_to(dotdot, dot)       # ".." at the parent, which is root
    spec_to(sub, sdot)         # a directory's spec is its first child

    # sub's chain: . -> .. -> deep -> end
    link_to(sdot, sdotdot)
    link_to(sdotdot, deep)
    link_to(deep, 0)
    spec_to(sdot, sub)
    spec_to(sdotdot, dot)      # back up to root: the loop a walker must not take

    return b.finish()


def main() -> int:
    aligned = build_image(page_align=True)
    fs = romfs.Romfs(aligned)

    check("volume name is read", fs.volume == "TEST", repr(fs.volume))

    paths = sorted(e.path for e in fs.entries)
    check("tree is walked without following . and ..",
          paths == ["/hello", "/link", "/sub", "/sub/deep"], repr(paths))

    hello = fs.find("/hello")
    check("file contents come back", hello is not None and
          fs.read(hello) == b"HELLO ROMFS\n")
    check("the executable bit is read", hello is not None and hello.execbit)

    lnk = fs.find("/link")
    check("a symlink is a symlink, and its target is its data",
          lnk is not None and lnk.kind == 3 and fs.read(lnk) == b"hello")

    check("a missing path is None", fs.find("/nope") is None)

    check("page-aligned image passes the XIP check",
          romfs.check_xip(fs) == [])

    # The case this tool exists for.
    unaligned = romfs.Romfs(build_image(page_align=False))
    bad = romfs.check_xip(unaligned)
    check("16-byte-aligned image fails the XIP check",
          len(bad) == 2, f"{len(bad)} offenders, expected 2")

    # A directory is not a mappable file and must not be reported.
    check("directories and symlinks are exempt from the XIP check",
          all("/sub" != line.split()[0] for line in bad), repr(bad))

    # And the CLI, since that is what the gates actually invoke.
    with tempfile.NamedTemporaryFile(suffix=".romfs", delete=False) as fh:
        fh.write(aligned)
        path = fh.name
    try:
        r = subprocess.run([sys.executable, os.path.join(HERE, "romfs.py"),
                            path, "--check", "--expect", "/hello", "--quiet"],
                           capture_output=True)
        check("CLI exits 0 on a good image", r.returncode == 0,
              r.stderr.decode(errors="replace").strip())

        r = subprocess.run([sys.executable, os.path.join(HERE, "romfs.py"),
                            path, "--expect", "/absent", "--quiet"],
                           capture_output=True)
        check("CLI exits 1 when an expected path is missing", r.returncode == 1)

        r = subprocess.run([sys.executable, os.path.join(HERE, "romfs.py"),
                            path, "--cat", "/link"], capture_output=True)
        check("CLI prints a symlink's target",
              b"-> hello" in r.stdout, r.stdout[:80].decode(errors="replace"))
    finally:
        os.unlink(path)

    # Cross-check against a real genromfs image when one is lying around.
    #
    # Everything above builds its own input, which leaves one gap: if this file
    # and romfs.py misread the format the same way, both agree and both are
    # wrong. genromfs is a third opinion. Skipped in CI, which has no build.
    real = os.path.join(HERE, "..", "build", "core2-slave", "rootfs.romfs")
    if os.path.exists(real):
        with open(real, "rb") as fh:
            rfs = romfs.Romfs(fh.read())
        check("a genromfs image parses at all", len(rfs.entries) > 50,
              f"{len(rfs.entries)} entries")
        check("and contains the shell everything else depends on",
              rfs.find("/bin/busybox") is not None)
        # genromfs is run with -a 4096 for exactly this reason, so a real image
        # failing here means the build lost the alignment, not that the parser
        # is wrong -- which is the whole point of having the check.
        check("and is page-aligned, as post-image.sh builds it",
              romfs.check_xip(rfs) == [],
              str(romfs.check_xip(rfs)[:2]))
    else:
        print("skip  no built image to cross-check against genromfs")

    if FAILURES:
        print(f"\nFAIL  romfs: {len(FAILURES)} check(s) failed")
        return 1
    print("\nPASS  romfs: parser and XIP check verified against built images")
    return 0


if __name__ == "__main__":
    sys.exit(main())
