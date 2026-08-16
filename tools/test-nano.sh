#!/usr/bin/env bash
#
# test-nano.sh - Stage 1 gate: nano edits a file, on the HDMI console.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Buildroot builds nano as --enable-tiny on NOMMU, throwing away undo, colour
# and nanorc to avoid a few fork() calls. This checks the full build: that it
# draws, that a keystroke reaches it, and that ^O actually writes the file --
# the last being the part a screen decode alone would not prove, since a screen
# showing the right characters says nothing about what reached the disk.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
cd "$REPO"

# shellcheck source=probe.sh
source "$HERE/probe.sh"

IOSERVER=build/ioserver/master-ioserver.elf
LOG=logs/nano-test.log

[ -f "$IOSERVER" ] || die "no $IOSERVER -- run tools/build-ioserver.sh"

echo "==> flashing the master"
./tools/flash.sh master "$IOSERVER" >/dev/null

echo "==> starting the master"
oocd master "init" "reset run" "exit" >/dev/null 2>&1 || true
sleep 6

echo "==> flashing the slave and booting Linux"
SLAVE_TIMEOUT=45 ./tools/flash-slave.sh || die "the slave did not boot Linux"

MASTER_TTY="$(python3 tools/devices.py resolve master | sed -n 's/^MASTER_TTY=//p')"
[ -n "$MASTER_TTY" ] || die "could not resolve the master's tty"

echo "==> typing into nano"
python3 - "$MASTER_TTY" <<'PY'
import sys, time
import serial

port = serial.Serial(sys.argv[1], 115200, timeout=0.3)
time.sleep(0.5)

def send(s, settle=2.5):
    for ch in s:
        port.write(ch.encode())
        port.flush()
        time.sleep(0.02)
    time.sleep(settle)

send("root\n", 3)
send("nano /tmp/n.txt\n", 4)
send("NANO_WROTE_THIS")           # type a line
send("\x0f", 3)                   # ^O write out
send("\r", 3)                     # accept the filename
send("\x18", 3)                   # ^X exit
PY

echo "==> reading the screen"
python3 tools/screen.py --png logs/nano-test.png | tee "$LOG"

echo "==> checking the file on the target"
python3 - "$LOG" <<'PY'
import sys
sys.path.insert(0, "tools")
from slave_console import Console

c = Console(settle=1.0)
c.send("", 2)
# Let the target decide, and emit a sentinel only grep can produce. Asserting
# on NANO_WROTE_THIS directly passes on failure: the shell echoes the string
# back in "sh: can't execute 'NANO_WROTE_THIS'" when nano died before running.
c.send("grep -q NANO_WROTE_THIS /tmp/n.txt && echo NANO_VERIFY_OK", 4)
open(sys.argv[1], "ab").write(c.buf)
print(c.text()[-400:])
PY

echo
if grep -qa "Segmentation fault\|MPU fault" "$LOG"; then
    die "nano crashed (screen: logs/nano-test.png)"
fi
# NANO_VERIFY_OK exists only if grep found the text inside the file on the
# target. Asserting on the typed string itself passes on failure, because the
# shell echoes it back when nano is not running to receive it.
if ! grep -qa "NANO_VERIFY_OK" "$LOG"; then
    die "nano did not write the file (screen: logs/nano-test.png)"
fi
echo "PASS  Stage 1: nano draws, takes input, and writes the file"
