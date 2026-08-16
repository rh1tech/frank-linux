#!/usr/bin/env bash
#
# enter-bootsel.sh <master|slave> - put a half into the ROM USB bootloader.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The way out of a wedged board without touching it.
#
# Holding BOOTSEL while powering on used to be the only route, and it is also
# the only recovery from a debug port that has stopped answering -- which on
# this board happens. That made the whole flash-and-test loop depend on somebody
# being in the room.
#
# The firmware now carries the vendor interface picotool looks for, so this
# works whether Linux is running, wedged, or the SWD link is dead. Once in the
# bootloader, picotool writes flash with SWD out of the picture entirely.
#
# Tried on the target's own USB, not through a probe: the probes are for SWD and
# this deliberately does not depend on SWD working.

set -euo pipefail

ROLE="${1:-slave}"

# The role is used in messages only, and deliberately: picotool addresses the
# one device that offers the interface, and today that is the slave's afboot.
# If the master ever carries it too, this has to start passing --bus/--address
# from tools/devices.py, because "the only compatible device" stops being an
# identity the moment there are two.

command -v picotool >/dev/null || { echo "picotool not found" >&2; exit 1; }

# A glob rather than `ls | grep`: the mount point is a fixed name and this is
# both cheaper and safe against odd filenames.
in_bootloader() { [ -d /Volumes/RP2350 ]; }

if in_bootloader; then
    echo "==> $ROLE is already in the bootloader"
    exit 0
fi

echo "==> asking $ROLE to reboot into the USB bootloader"
if ! picotool reboot -u -f >/dev/null 2>&1; then
    cat >&2 <<'MSG'
error: could not reach the reset interface.

       That interface is provided by our own firmware (firmware/common/
       usb_descriptors.c), so this fails when the half is running something
       older than it, or nothing at all. Hold BOOTSEL on that half and power
       the board on -- once, to install firmware that will not need it again.
MSG
    exit 1
fi

for _ in $(seq 1 20); do
    if in_bootloader; then
        echo "==> $ROLE is in the bootloader"
        exit 0
    fi
    sleep 0.5
done
echo "error: $ROLE did not appear as a bootloader device" >&2
exit 1
