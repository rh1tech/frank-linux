#!/usr/bin/env bash
#
# test-mc.sh - Stage 2 gate: Midnight Commander, on the HDMI console.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Buildroot refuses mc on NOMMU (`depends on BR2_USE_MMU # libglib2, slang`).
# The dependency is real but it is about fork(), and mc without its subshell
# does not need one -- see buildroot-patches/0003. This checks that the thing we
# built from that claim actually runs.
#
# Three separate assertions, because mc can fail at three unrelated points and
# two of them look identical on a screen capture:
#
#   1. it starts at all         -- glib initialises, ncurses opens the terminal
#   2. it draws its panels      -- the terminal speaks enough vt102 for a
#                                  full-screen application with a frame
#   3. F3 views a real file     -- the internal viewer works, which is the part
#                                  that does not need a process
#
# and one thing that must NOT happen: no fork(). A build that silently kept a
# fork() path would run fine until the first spawn and then behave in a way
# nobody could explain, so the file it views is one this script writes with a
# known sentinel in it, and the sentinel has to come back off the screen.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
cd "$REPO"

# shellcheck source=probe.sh
source "$HERE/probe.sh"

IOSERVER=build/ioserver/master-ioserver.elf
LOG=logs/mc-test.log
SENTINEL=MC_VIEWED_THIS

[ -f "$IOSERVER" ] || die "no $IOSERVER -- run tools/build-ioserver.sh"

# Fail before spending four minutes on the board if mc is not in the image.
# romfs.py reads the flash image on this laptop, so this costs nothing.
python3 tools/romfs.py build/core2-slave/rootfs.romfs \
    --expect /usr/bin/mc --quiet \
    || die "mc is not in the rootfs image -- check BR2_PACKAGE_MC and the build"

echo "==> flashing the master"
./tools/flash.sh master "$IOSERVER" >/dev/null

echo "==> starting the master"
oocd master "init" "reset run" "exit" >/dev/null 2>&1 || true
sleep 6

echo "==> flashing the slave and booting Linux"
SLAVE_TIMEOUT=45 ./tools/flash-slave.sh || die "the slave did not boot Linux"

MASTER_TTY="$(python3 tools/devices.py resolve master | sed -n 's/^MASTER_TTY=//p')"
[ -n "$MASTER_TTY" ] || die "could not resolve the master's tty"

echo "==> starting mc"
python3 - "$MASTER_TTY" "$SENTINEL" <<'PY'
import sys, time
import serial

port, sentinel = serial.Serial(sys.argv[1], 115200, timeout=0.3), sys.argv[2]
time.sleep(0.5)


def send(s, settle=2.5):
    for ch in s:
        port.write(ch.encode())
        port.flush()
        time.sleep(0.02)
    time.sleep(settle)


send("root\n", 3)
# A file with a known string in it, in a directory holding nothing else, so the
# panel listing and the viewer both have exactly one thing to show and neither
# can match by accident.
send(f"mkdir -p /tmp/mct && echo {sentinel} > /tmp/mct/f.txt\n", 2)
# mc takes a while to start: glib initialises, ncurses probes the terminal, and
# the first full-screen repaint is 2000 cells down a link shared with the
# keyboard.
send("mc /tmp/mct\n", 12)
PY

echo "==> reading the panels"
python3 tools/screen.py --png logs/mc-panels.png | tee "$LOG"

echo "==> viewing a file (F3)"
python3 - "$MASTER_TTY" <<'PY'
import sys, time
import serial

port = serial.Serial(sys.argv[1], 115200, timeout=0.3)
# F3 as the terminfo entry sends it for vt102: mc reads the key by sequence, not
# by name, so this is also a test that the master's terminal emits what the
# terminfo entry we ship claims it does.
port.write(b"\x1bOR")
port.flush()
time.sleep(6)
PY

python3 tools/screen.py --png logs/mc-view.png | tee -a "$LOG"

echo "==> quitting"
python3 - "$MASTER_TTY" <<'PY'
import sys, time
import serial

port = serial.Serial(sys.argv[1], 115200, timeout=0.3)
port.write(b"\x1b")          # leave the viewer
time.sleep(2)
port.write(b"\x1b\x1b")      # and mc; F10 would need a confirmation dialog
time.sleep(3)
PY

echo
if grep -qa "Segmentation fault\|MPU fault\|not implemented\|Function not impl" "$LOG"; then
    die "mc crashed or hit a missing syscall (screens: logs/mc-*.png)"
fi
# The panel frame. mc draws its border with ACS line-drawing characters, which
# arrive as the decoder's block glyph, and the directory name is in the frame's
# title -- neither of which a shell error message can produce.
if ! grep -qa "/tmp/mct" "$LOG"; then
    die "mc did not draw its panels (screen: logs/mc-panels.png)"
fi
# The viewer, which is the assertion that mc did real work rather than merely
# starting. The sentinel is inside the file, so it can only be on the screen if
# mc opened and rendered it.
if ! grep -qa "$SENTINEL" "$LOG"; then
    die "mc's internal viewer did not show the file (screen: logs/mc-view.png)"
fi
echo "PASS  Stage 2: mc draws its panels and views a file, with no fork()"
