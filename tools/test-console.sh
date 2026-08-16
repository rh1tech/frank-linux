#!/usr/bin/env bash
#
# test-console.sh - Phase 6 gate: a shell on HDMI, driven by typing.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The whole project's through-line, made on the whole system at once: log in,
# run a command, and read the answer off the HDMI capture card. Every layer is
# in that loop -- the terminal engine, the link, ttyFRK0, the shell, DispHSTX,
# and the font decode.
#
# Keystrokes are typed into the master's own UART, which the harness reaches
# through the probe. They take the identical path a USB keypress takes, from
# terminal_feed_event() onward. What this cannot press is a real USB key, so the
# HID host is exercised only as far as "it is initialised and does not break the
# rest"; a physical keyboard is the one thing here no test can stand in for.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
cd "$REPO"

# shellcheck source=probe.sh
source "$HERE/probe.sh"

IOSERVER=build/ioserver/master-ioserver.elf
LOG=logs/console-test.log

[ -f "$IOSERVER" ] || die "no $IOSERVER -- run tools/build-ioserver.sh"

echo "==> flashing the master (terminal + HDMI + link + card)"
./tools/flash.sh master "$IOSERVER" >/dev/null

echo "==> starting the master"
oocd master "init" "reset run" "exit" >/dev/null 2>&1 || true
# It needs about 4.5 s to bring up display, keyboard and card before it can
# answer. The slave retries regardless, so this is only to save the retries.
sleep 6

# Flashing leaves the slave running, so Linux is started by the flash itself
# rather than by a warm reset afterwards. That matters: a warm `reset run` on a
# slave whose core 1 is mid-transaction has repeatedly left the chip in a state
# where openocd cannot even examine it, and which survives further resets --
# only the ROM rescue clears it. Flashing rescues first, so this path is the one
# that reliably starts from a known state.
echo "==> flashing the slave and booting Linux"
# Not piped through tail: a failure here is the most likely thing to go wrong,
# and hiding all but the last three lines of it costs more than the tidier
# output is worth.
SLAVE_TIMEOUT=45 ./tools/flash-slave.sh || die "the slave did not boot Linux"
cp logs/slave-boot.log logs/console-test-usb.log

echo "==> typing"
MASTER_TTY="$(python3 tools/devices.py resolve master | sed -n 's/^MASTER_TTY=//p')"
[ -n "$MASTER_TTY" ] || die "could not resolve the master's tty"

python3 - "$MASTER_TTY" <<'PY'
import sys, time
import serial

port = serial.Serial(sys.argv[1], 115200, timeout=0.3)
time.sleep(0.5)
for line in ["root\n", "echo FRANK_KEYS_OK; uname -m\n"]:
    for ch in line:
        port.write(ch.encode())
        port.flush()
        time.sleep(0.02)      # a plausible typing rate, not a burst
    time.sleep(2.5)
time.sleep(2)
PY

echo "==> reading the screen"
python3 tools/screen.py --png logs/console-test.png | tee "$LOG"

echo
# Assert on what only the full loop can produce. FRANK_KEYS_OK proves the
# keystrokes reached a shell, and "armv7ml" proves the shell that answered is
# the one running on the Cortex-M33 -- neither string exists anywhere on the
# master, so neither can be echoed back by a half-working path.
if ! grep -qa "FRANK_KEYS_OK" "$LOG"; then
    die "typed command never reached the shell (screen: logs/console-test.png)"
fi
if ! grep -qa "armv7ml" "$LOG"; then
    die "no command output came back to the screen (screen: logs/console-test.png)"
fi
# The banner, which getty renders from /etc/issue before the prompt. It is the
# only part of the screen that comes from this project's own files rather than
# from Buildroot's skeleton, so it is the part that proves the image on the
# board is ours. It said "buildroot login" until the machine was given a name,
# and this check went on passing on a stale assumption for exactly as long as
# nothing changed.
if ! grep -qa "FRANK Linux v" "$LOG" || ! grep -qa "frank login" "$LOG"; then
    die "no FRANK banner or login prompt on screen (screen: logs/console-test.png)"
fi
echo "PASS  Phase 6: a shell on HDMI, answering the keyboard"
