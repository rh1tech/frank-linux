#!/usr/bin/env python3
"""
romfs.py - read the flash rootfs image on the host.

SPDX-License-Identifier: GPL-3.0-or-later

The romfs in flash is the root filesystem, so almost every question about the
running system's userspace -- is this file in the image, does it have the right
mode, did post-build.sh actually rewrite the fstab -- is a question about a file
sitting on this laptop. Answering it by flashing both halves and reading the
answer off an HDMI capture takes four minutes and a working debug port.

The format is small enough to parse in one screen of code, so answer it here.

The alignment check is the reason this exists rather than `mount -o loop`. On
NOMMU a program's text is executed in place only if the filesystem can hand the
kernel a mapping the MMU-less hardware can use directly, which means the data
has to start on a page boundary. genromfs aligns to 16 bytes by default, which
is legal romfs, produces a working system, and silently copies every binary into
RAM instead -- the difference between 384 kB of BusyBox costing nothing and
costing 384 kB. Nothing in the image or the boot log says which one you have.

Usage:
    romfs.py IMAGE                    list the tree
    romfs.py IMAGE --cat /etc/fstab   print a file
    romfs.py IMAGE --check            verify it is XIP-able, exit 1 if not
    romfs.py IMAGE --expect /bin/sh   assert a path exists; repeatable
"""

from __future__ import annotations

import argparse
import struct
import sys

MAGIC = b"-rom1fs-"
PAGE = 4096

# Low three bits of the header's first word.
TYPES = {
    0: "hardlink",
    1: "dir",
    2: "file",
    3: "symlink",
    4: "blockdev",
    5: "chardev",
    6: "socket",
    7: "fifo",
}
EXEC_BIT = 0x8


def align16(n: int) -> int:
    return (n + 15) & ~15


def cstr(buf: bytes, off: int) -> str:
    end = buf.index(b"\0", off)
    return buf[off:end].decode("utf-8", "replace")


class Entry:
    def __init__(self, path: str, kind: int, execbit: bool,
                 size: int, data_off: int, spec: int):
        self.path = path
        self.kind = kind
        self.execbit = execbit
        self.size = size
        self.data_off = data_off
        self.spec = spec

    @property
    def type_name(self) -> str:
        return TYPES.get(self.kind, "?")

    @property
    def is_file(self) -> bool:
        return self.kind == 2


class Romfs:
    def __init__(self, blob: bytes):
        if blob[:8] != MAGIC:
            sys.exit("not a romfs image (bad magic)")
        self.blob = blob
        self.size = struct.unpack_from(">I", blob, 8)[0]
        if self.size > len(blob):
            sys.exit(f"image claims {self.size} bytes but the file is {len(blob)}")
        self.volume = cstr(blob, 16)
        first = align16(16 + len(self.volume) + 1)
        self.entries: list[Entry] = []
        self._walk(first, "")

    def _walk(self, off: int, prefix: str) -> None:
        """
        Follow one directory's sibling chain.

        The chain ends when the next-pointer is zero, and every directory begins
        with "." and ".." entries whose next-pointers lead back up. Recursing
        into those would not terminate, so they are skipped by name -- and
        `seen` guards the rest, because a malformed image should give a wrong
        listing rather than hang.
        """
        seen: set[int] = set()
        while off and off not in seen:
            seen.add(off)
            if off + 16 > len(self.blob):
                sys.exit(f"header at {off:#x} runs past the end of the image")
            nextw, spec, size, _csum = struct.unpack_from(">IIII", self.blob, off)
            name = cstr(self.blob, off + 16)
            data = align16(off + 16 + len(name) + 1)
            kind = nextw & 0x7
            ent = Entry(f"{prefix}/{name}", kind, bool(nextw & EXEC_BIT),
                        size, data, spec)

            if name not in (".", ".."):
                self.entries.append(ent)
                if kind == 1:
                    self._walk(spec, ent.path)

            off = nextw & ~0xF

    def find(self, path: str) -> Entry | None:
        want = path.rstrip("/") or "/"
        for e in self.entries:
            if e.path == want:
                return e
        return None

    def read(self, ent: Entry) -> bytes:
        return self.blob[ent.data_off:ent.data_off + ent.size]


def check_xip(fs: Romfs) -> list[str]:
    """
    Every regular file's data must start on a page boundary.

    Directories, symlinks and device nodes are exempt: nothing maps them, and
    padding each one to 4 kB would multiply the size of the image for no gain.
    Empty files are exempt too -- there is nothing to map, and genromfs does not
    pad them.
    """
    bad = []
    for e in fs.entries:
        if e.is_file and e.size and e.data_off % PAGE:
            bad.append(f"{e.path} at {e.data_off:#x} (+{e.data_off % PAGE} into a page)")
    return bad


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    ap.add_argument("--cat", metavar="PATH")
    ap.add_argument("--check", action="store_true",
                    help="verify every file is page-aligned for execute-in-place")
    ap.add_argument("--expect", action="append", default=[], metavar="PATH",
                    help="assert this path is in the image; repeatable")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    with open(args.image, "rb") as fh:
        fs = Romfs(fh.read())

    if args.cat:
        ent = fs.find(args.cat)
        if ent is None:
            sys.exit(f"{args.cat}: not in the image")
        if ent.kind == 3:
            print(f"{args.cat} -> {fs.read(ent).decode(errors='replace')}")
            return 0
        if not ent.is_file:
            sys.exit(f"{args.cat}: is a {ent.type_name}")
        sys.stdout.buffer.write(fs.read(ent))
        return 0

    rc = 0

    if args.expect:
        for path in args.expect:
            if fs.find(path) is None:
                print(f"FAIL  romfs: {path} is not in the image", file=sys.stderr)
                rc = 1

    if args.check:
        bad = check_xip(fs)
        if bad:
            print(f"FAIL  romfs: {len(bad)} file(s) not page-aligned -- these "
                  f"will be copied into RAM, not executed in place:", file=sys.stderr)
            for line in bad[:10]:
                print(f"        {line}", file=sys.stderr)
            if len(bad) > 10:
                print(f"        ... and {len(bad) - 10} more", file=sys.stderr)
            print("      genromfs needs -a 4096.", file=sys.stderr)
            rc = 1

    if not args.quiet and not args.expect and not args.check:
        for e in sorted(fs.entries, key=lambda e: e.path):
            mark = "*" if e.execbit else " "
            extra = ""
            if e.kind == 3:
                extra = " -> " + fs.read(e).decode(errors="replace")
            print(f"{e.type_name:8} {mark} {e.size:9} {e.data_off:#010x} "
                  f"{e.path}{extra}")

    if not args.quiet:
        files = sum(1 for e in fs.entries if e.is_file)
        print(f"\n[volume {fs.volume!r}, {fs.size} bytes, "
              f"{len(fs.entries)} entries, {files} files]", file=sys.stderr)
        if rc == 0 and (args.check or args.expect):
            print("PASS  romfs: image checks out", file=sys.stderr)

    return rc


if __name__ == "__main__":
    sys.exit(main())
