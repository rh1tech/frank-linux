#!/usr/bin/env bash
#
# test-blk.sh - Phase 5 gate: the microSD, served to Linux over the link.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The card is on the master's SPI0; the half that runs Linux has no slot. This
# flashes both halves, boots Linux and checks that the card is really there.
#
# Mounted READ-ONLY, deliberately. This is a real card with real data on it, and
# a gate that can destroy the thing it is testing is not a gate. Writes are
# proven separately against the master's RAM disk (tools/test-blk-write.sh).

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
cd "$REPO"

# shellcheck source=probe.sh
source "$HERE/probe.sh"

IOSERVER=build/ioserver/master-ioserver.elf
LOG=logs/blk-test.log

[ -f "$IOSERVER" ] || die "no $IOSERVER -- run tools/build-ioserver.sh"

echo "==> flashing the master I/O server"
./tools/flash.sh master "$IOSERVER" >/dev/null

# The master must be serving before the slave's core 1 asks it for a capacity.
echo "==> starting the master"
oocd master "init" "reset run" "exit" >/dev/null 2>&1 || true
# It needs about 4.5 s to bring up display, keyboard and card before it can
# answer. The slave retries regardless, so this is only to save the retries.
sleep 6

# Flashing leaves the slave running, so Linux is started by the flash rather
# than by a warm reset afterwards. A warm `reset run` on a slave whose core 1 is
# mid-transaction has repeatedly left the chip in a state where openocd cannot
# examine it at all, and which survives further resets.
echo "==> flashing the slave and booting Linux"
SLAVE_TIMEOUT=45 ./tools/flash-slave.sh || die "the slave did not boot Linux"

echo "==> testing the disk"
# The driver's registration line and the mount are both in the boot log, because
# Linux was already running by the time this attaches and /etc/init.d/S30sd
# mounts the card at boot. The session below reads the result rather than
# repeating the mount -- doing it again gets "Can't open blockdev", since the
# device is already held, which is a success that looks exactly like a failure.
cp logs/slave-boot.log "$LOG"
python3 - "$LOG" <<'PY'
import sys
sys.path.insert(0, "tools")
from slave_console import Console

log = sys.argv[1]
c = Console(settle=1.0)
c.send("", 2)
c.send("root", 3)
c.send("cat /sys/block/frankblk0/size")
c.send("mount | grep /mnt/sd", 4)
c.send("ls /mnt/sd | head -20", 6)
# Read a real byte off the card rather than trusting the directory listing:
# a listing comes out of the FAT driver's own caches, and the point of this
# gate is that sectors crossed the link.
# tail -1, not head -1: the signature is the last two bytes of the sector, and
# -v so od does not fold repeated lines and take the last one with it.
c.send("dd if=/dev/frankblk0p1 bs=512 count=1 2>/dev/null | od -v -An -tx1 | tail -1", 6)
c.send("echo BLKTEST_DONE", 3)

open(log, "ab").write(c.buf)
print(c.text()[-2500:])
PY

echo
# Assert on things only a working card can produce.
#
# Two earlier versions of this check passed on failure: "frankblk0" matched
# "ls: /dev/frankblk0: No such file or directory", and the next attempt matched
# the shell echoing the command back. Both are the same mistake -- grepping for
# a string that appears whether or not the thing worked.
#
# These cannot be faked by an error message:
#   - the driver prints its size line only after add_disk() succeeds
#   - "frankblk0: p1" means the block layer parsed a partition table that came
#     over the link, so real sectors were read
#   - the mount line comes from /proc/mounts, so the kernel is the one saying it
if ! grep -qa "sectors .* over the link" "$LOG"; then
    die "frank-blk did not register a disk (log: $LOG)"
fi
if ! grep -qa "frankblk0: p1" "$LOG"; then
    die "no partition table read off the card (log: $LOG)"
fi
if ! grep -qa "BLKTEST_DONE" "$LOG"; then
    die "shell did not respond (log: $LOG)"
fi
if ! grep -qa "Mounting the SD card: OK" "$LOG"; then
    die "the card was not mounted at boot (log: $LOG)"
fi
# From /proc/mounts via `mount`, so this is the kernel's account of the mount
# and not the init script's opinion of what it did.
if ! grep -qa "/dev/frankblk0p1 on /mnt/sd type vfat" "$LOG"; then
    die "the card is not mounted where it should be (log: $LOG)"
fi
# The MBR/VBR signature at the end of sector 0. Every FAT volume has it, and no
# error message on this console contains "55 aa" -- so this is proof that 512
# real bytes came off the card, through the master, over the link, and into a
# process running on the other chip.
if ! grep -qa "55 aa" "$LOG"; then
    die "no boot signature in sector 0 -- sectors are not really being read (log: $LOG)"
fi
echo "PASS  Phase 5: microSD mounted at boot and read over the link"
