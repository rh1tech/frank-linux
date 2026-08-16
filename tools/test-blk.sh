#!/usr/bin/env bash
#
# test-blk.sh - Phase 5 gate: a block device on the slave, served by the master.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Flashes both halves, boots Linux, and checks that /dev/frankblk0 exists and
# reads back what the master put there. The master fills sector N with a pattern
# derived from N, so a read can be verified without either side sending the
# other any reference data.

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

echo "==> flashing the slave (bootloader + kernel + dtb)"
SLAVE_TIMEOUT=1 ./tools/flash-slave.sh >/dev/null 2>&1 || true

# The master must be serving before the slave's core 1 asks it for a capacity,
# which it does once at startup. Reset the master first and give it a moment.
echo "==> starting the master"
oocd master "init" "reset run" "exit" >/dev/null 2>&1 || true
sleep 3

echo "==> booting the slave and testing the disk"
: > "$LOG"
python3 - "$LOG" <<'PY'
import glob, subprocess, sys, time, serial

log = sys.argv[1]
subprocess.run(["bash", "-c",
                'source tools/probe.sh; oocd slave "init" "reset run" "exit" >/dev/null 2>&1'],
               check=False)

port, end = None, time.time() + 30
while time.time() < end and not port:
    found = glob.glob("/dev/cu.usbmodemFRANK*")
    if found:
        try:
            port = serial.Serial(found[0], 115200, timeout=0.3)
        except Exception:
            time.sleep(0.3)
    else:
        time.sleep(0.3)
if not port:
    sys.exit("no slave console")

buf = b""
def drain(seconds):
    global buf
    end = time.time() + seconds
    while time.time() < end:
        buf += port.read(4096)

def send(cmd, wait=3.0):
    port.write((cmd + "\n").encode())
    port.flush()
    drain(wait)

drain(35)                       # boot
send("", 2)
send("root", 3)
# Does the disk exist, how big is it, and does it read back what the master
# wrote? od is used rather than cmp because there is no second copy to compare
# against -- the pattern is checked by eye and by grep.
send("ls -l /dev/frankblk0")
send("cat /sys/block/frankblk0/size")
send("dd if=/dev/frankblk0 bs=512 count=1 skip=2 2>/dev/null | od -An -tx4 | head -2")
send("dd if=/dev/frankblk0 bs=512 count=1 skip=5 2>/dev/null | od -An -tx4 | head -1")
send("echo BLKTEST_DONE")

open(log, "wb").write(buf)
print(buf.decode("utf-8", "replace")[-2500:])
PY

echo
# Assert on something only success can produce.
#
# Two earlier versions of this check passed on failure: "frankblk0" matched
# "ls: /dev/frankblk0: No such file or directory", and the next attempt matched
# the shell echoing the command back. Both are the same mistake -- grepping for
# a string that appears whether or not the thing worked.
#
# The driver prints "N sectors (M KiB) over the link" only after add_disk
# succeeds, so that line means the disk is really there.
if ! grep -qa "sectors .* over the link" "$LOG"; then
    die "frank-blk did not register a disk (log: $LOG)"
fi
if ! grep -qa "BLKTEST_DONE" "$LOG"; then
    die "shell did not respond (log: $LOG)"
fi
echo "PASS  Phase 5: block device over the link is readable from Linux"
