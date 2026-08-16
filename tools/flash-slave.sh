#!/usr/bin/env bash
#
# flash-slave.sh - the Phase 4 gate: ARM Linux on the FRANK Core 2 Proto slave.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Flash layout, agreed with firmware/afboot-rp2350/src/main.c:
#
#   0x10000000  afboot-rp2350          the bootloader, up to 512 kB
#   0x100f0000  DTB    + 8-byte header
#   0x10100000  Image  + 8-byte header
#   0x10800000  rootfs.romfs           raw, no header -- the kernel reads it in
#                                      place through MTD, so nothing copies it
#
# The header is magic ("FRPL") and a length. Without a length the bootloader
# would have to copy the whole flash window into PSRAM and hope; with it, it
# copies exactly what is there and can tell an erased slot from a real payload.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
cd "$REPO"

# shellcheck source=probe.sh
source "$HERE/probe.sh"

IMAGES=build/core2-slave
BOOT=build/afboot/afboot-rp2350.elf
LOG=logs/slave-boot.log
TIMEOUT="${SLAVE_TIMEOUT:-120}"

DTB_ADDR=0x100f0000
KERNEL_ADDR=0x10100000
# Matches the "rootfs" partition in the device tree. Written raw: this one is
# never copied anywhere, the kernel maps it where it lies.
ROMFS_ADDR=0x10800000
ROMFS=build/core2-slave/rootfs.romfs

[ -f "$BOOT" ] || die "no $BOOT -- run tools/build-afboot.sh"
KERNEL="$IMAGES/Image"
DTB="$(find "$IMAGES" -maxdepth 1 -name '*.dtb' -print -quit 2>/dev/null)"
[ -f "$KERNEL" ] || die "no $KERNEL -- run 'make slave'"
[ -n "$DTB" ] || die "no DTB in $IMAGES -- run 'make slave'"

mkdir -p build/payload logs

# Wrap a file with the header afboot looks for.
wrap() {
    python3 - "$1" "$2" <<'PY'
import struct, sys
src, dst = sys.argv[1], sys.argv[2]
data = open(src, "rb").read()
with open(dst, "wb") as f:
    f.write(struct.pack("<II", 0x4c505246, len(data)))   # 'FRPL', length
    f.write(data)
print(f"    {src}: {len(data)} bytes")
PY
}

echo "==> wrapping payloads"
wrap "$DTB" build/payload/dtb.bin
wrap "$KERNEL" build/payload/kernel.bin

echo "==> flashing slave"
./tools/flash.sh slave "$BOOT" >/dev/null
./tools/flash.sh slave "build/payload/dtb.bin@$DTB_ADDR" >/dev/null
./tools/flash.sh slave "build/payload/kernel.bin@$KERNEL_ADDR" >/dev/null
if [ -f "$ROMFS" ]; then
    ./tools/flash.sh slave "$ROMFS@$ROMFS_ADDR" >/dev/null
fi
echo "    bootloader + dtb + kernel${ROMFS:+ + romfs} written"

# Reset first, then attach.
#
# The opposite order does not work: opening the port and then resetting the chip
# disconnects USB, invalidating the file descriptor
# ("OSError: [Errno 6] Device not configured"). afboot waits up to ten seconds
# for DTR precisely so a reader can arrive after the reset, and core 1 holds the
# ring until it does.
: > "$LOG"
echo "==> resetting slave"
oocd slave "init" "reset run" "exit" >/dev/null 2>&1 || true

echo "==> attaching to the console"
python3 - "$LOG" "$TIMEOUT" <<'PY' &
import glob, sys, time, serial
log, timeout = sys.argv[1], float(sys.argv[2])
port, end = None, time.time() + 25
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
    sys.exit(1)
deadline = time.time() + timeout
with open(log, "ab", buffering=0) as f:
    while time.time() < deadline:
        try:
            d = port.read(4096)
        except Exception:
            break
        if d:
            f.write(d)
PY
CAP=$!
cleanup() { kill "$CAP" 2>/dev/null || true; }
trap cleanup EXIT

PROMPT='~ #|/ #|login:|Welcome to Buildroot'
DEADLINE=$(( $(date +%s) + TIMEOUT ))
while [ "$(date +%s)" -lt "$DEADLINE" ]; do
    grep -qaE "$PROMPT" "$LOG" && break
    sleep 1
done
cleanup
sleep 0.3

echo
cat "$LOG"
echo

if ! grep -qa 'afboot-rp2350' "$LOG"; then
    die "bootloader did not run (log: $LOG)"
fi
if ! grep -qa 'Linux version' "$LOG"; then
    die "no kernel banner -- handover, DTB or console (log: $LOG)"
fi
if ! grep -qaE "$PROMPT" "$LOG"; then
    die "kernel booted but no shell -- initramfs or FDPIC loader (log: $LOG)"
fi

echo "PASS  Phase 4: ARM Linux booted to a shell on the FRANK slave half"
