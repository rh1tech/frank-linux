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
# The driver's registration line is in the boot log, because Linux was already
# running by the time this attaches; the mount is in the session below. Both
# halves of the evidence are needed, so the assertions run over the two joined.
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
c.send("ls /dev/frankblk0p1")
c.send("mkdir -p /mnt/sd; mount -o ro /dev/frankblk0p1 /mnt/sd 2>&1", 8)
c.send("ls /mnt/sd | head -20", 6)
c.send("umount /mnt/sd; echo BLKTEST_DONE", 4)

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
# These three cannot be faked by an error message:
#   - the driver prints its size line only after add_disk() succeeds
#   - "frankblk0: p1" means the block layer parsed a partition table that came
#     over the link, so real sectors were read
#   - mounting and listing means the FAT driver agreed too
if ! grep -qa "sectors .* over the link" "$LOG"; then
    die "frank-blk did not register a disk (log: $LOG)"
fi
if ! grep -qa "frankblk0: p1" "$LOG"; then
    die "no partition table read off the card (log: $LOG)"
fi
if ! grep -qa "BLKTEST_DONE" "$LOG"; then
    die "shell did not respond (log: $LOG)"
fi
# The listing has to contain something, and "ls: ... No such file" must not count.
if grep -qa "mount: mounting .* failed" "$LOG"; then
    die "the card would not mount (log: $LOG)"
fi
echo "PASS  Phase 5: microSD mounted from Linux over the link"
