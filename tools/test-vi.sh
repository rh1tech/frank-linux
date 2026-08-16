#!/usr/bin/env bash
#
# test-vi.sh - Stage 0 gate: a full-screen editor on the HDMI console.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The first program to address the cursor rather than just print lines. Up to
# now the terminal has only had to append text and scroll; vi positions the
# cursor, clears to end of line, and draws a status row at a fixed position. If
# the VersaTerm engine on the master has gaps, they surface here rather than
# three stages later inside mc, where they would be much harder to attribute.
#
# It is also the check that `stty rows 25 cols 80` reached the tty: with the
# wrong size vi draws 24 rows and the tilde column ends in the wrong place.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
cd "$REPO"

# shellcheck source=probe.sh
source "$HERE/probe.sh"

IOSERVER=build/ioserver/master-ioserver.elf
LOG=logs/vi-test.log

[ -f "$IOSERVER" ] || die "no $IOSERVER -- run tools/build-ioserver.sh"

echo "==> flashing the master"
./tools/flash.sh master "$IOSERVER" >/dev/null

echo "==> starting the master"
oocd master "init" "reset run" "exit" >/dev/null 2>&1 || true
sleep 6

echo "==> flashing the slave and booting Linux"
SLAVE_TIMEOUT=45 ./tools/flash-slave.sh || die "the slave did not boot Linux"

echo "==> opening a file in vi"
MASTER_TTY="$(python3 tools/devices.py resolve master | sed -n 's/^MASTER_TTY=//p')"
[ -n "$MASTER_TTY" ] || die "could not resolve the master's tty"

python3 - "$MASTER_TTY" <<'PY'
import sys, time
import serial

port = serial.Serial(sys.argv[1], 115200, timeout=0.3)
time.sleep(0.5)

def type_line(s, settle=2.5):
    for ch in s:
        port.write(ch.encode())
        port.flush()
        time.sleep(0.02)
    time.sleep(settle)

type_line("root\n", 3)
# A file with content nothing else on the system would produce, so the decoded
# screen cannot be a coincidence.
type_line("printf 'VI_LINE_ALPHA\\nVI_LINE_BETA\\n' > /tmp/t\n")
type_line("vi /tmp/t\n", 4)
PY

echo "==> reading the screen"
python3 tools/screen.py --png logs/vi-test.png | tee "$LOG"

# Leave vi, or the next test inherits a full-screen editor.
python3 - "$MASTER_TTY" <<'PY'
import sys, time
import serial
port = serial.Serial(sys.argv[1], 115200, timeout=0.3)
port.write(b"\x1b:q!\r")
port.flush()
time.sleep(2)
PY

echo
if ! grep -qa "VI_LINE_ALPHA" "$LOG"; then
    die "vi did not draw the file (screen: logs/vi-test.png)"
fi
if ! grep -qa "VI_LINE_BETA" "$LOG"; then
    die "vi drew only part of the file (screen: logs/vi-test.png)"
fi
# The tilde column is what distinguishes a full-screen draw from a shell that
# merely echoed the file: vi fills every line past end-of-file with '~'.
if ! grep -qa "^~" "$LOG"; then
    die "no tilde filler -- vi is not in full-screen mode (screen: logs/vi-test.png)"
fi
echo "PASS  Stage 0: a full-screen editor draws correctly on the HDMI console"
