#!/bin/sh
#
# Build the romfs image that the board executes from.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Buildroot has no romfs target, and the alternatives do not do what is needed
# here. The point is not size, it is execute-in-place: QMI keeps the 16 MB QSPI
# flash mapped read-only at 0x10000000, and romfs stores files uncompressed and
# contiguously, so fs/romfs/mmap-nommu.c can hand a process a pointer directly
# into flash. Program text then costs no RAM at all -- no copy per exec, and one
# libc.so serving every process without being duplicated.
#
# That matters on a machine with 8 MB of PSRAM. The same userspace in a
# RAM-resident initramfs spends about 2 MB of it permanently, and one on the SD
# card would have to be copied in per process, libc included, because a
# block-backed filesystem cannot be pointed at either.

set -e

BINARIES_DIR="$1"
[ -n "$BINARIES_DIR" ] || { echo "post-image: no binaries dir given" >&2; exit 1; }
[ -n "$TARGET_DIR" ] || { echo "post-image: TARGET_DIR not set" >&2; exit 1; }

# -a 4096 is the option the whole exercise depends on.
#
# romfs_get_unmapped_area() hands MTD the file's data offset within the image
# and returns whatever address comes back. NOMMU will only accept a direct
# mapping at a page-aligned address, so with the default 16-byte alignment
# almost every file is rejected and the kernel quietly copies it into RAM
# instead -- no error, no message, just a userspace that costs as much memory as
# it would have from a block device. Aligning file data to pages is what makes
# execute-in-place actually happen.
#
# It costs padding: up to 4 kB per regular file.
#
# -d: directory to pack. -V: volume name, which shows up in dmesg on mount and
# is the cheapest way to confirm the board is running the image you just built.
genromfs -a 4096 -d "$TARGET_DIR" -f "$BINARIES_DIR/rootfs.romfs" -V "FRANK"

size=$(( $(stat -c %s "$BINARIES_DIR/rootfs.romfs") ))
echo "post-image: rootfs.romfs ${size} bytes ($((size / 1024)) kB)"

# The partition in the device tree is 11 MB at flash offset 0x500000, and the
# flash ends at 16 MB. Overflowing runs off the end of the chip, so say so here
# rather than let flash.sh write past it and leave a rootfs that mounts and is
# then truncated somewhere in the middle of a file.
limit=$((11 * 1024 * 1024))
if [ "$size" -gt "$limit" ]; then
    echo "post-image: rootfs.romfs is ${size} bytes, larger than the 11 MB" \
         "rootfs partition -- see the flash map in tools/flash-slave.sh" >&2
    exit 1
fi
exit 0
