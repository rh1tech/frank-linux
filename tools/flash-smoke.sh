#!/usr/bin/env bash
#
# flash-smoke.sh - Phase 1 gate: RISC-V Linux on the master half.
#
# Proves the board can host a Linux kernel in PSRAM before we write an ARM port
# that has never been done. If PSRAM timing, QMI setup or the flash layout is
# wrong, we find out here, in somebody else's known-good code, and we keep a
# working reference to diff against when the ARM kernel misbehaves.
#
# Runs on the master because that is the half whose UART console reaches a
# probe. Upstream's console is already UART0 on GPIO0/1, which is exactly the
# master's J2 header; on the slave GPIO0 is the PSRAM chip select, so UART0 does
# not exist there at all.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
cd "$REPO"

# shellcheck source=probe.sh
source "$HERE/probe.sh"

ROLE="${SMOKE_ROLE:-master}"
IMAGE="build/smoke-riscv/flash-image.bin"
# Flash XIP base. The image is bootloader + DTB + Image concatenated by
# genimage, laid out to be executed in place from the start of flash.
LOAD=0x10000000

[ -f "$IMAGE" ] || die "no $IMAGE -- run 'make smoke-riscv' first"

echo "==> $ROLE <- $IMAGE ($(stat -f%z "$IMAGE" 2>/dev/null || stat -c%s "$IMAGE") bytes)"

# Once a RISC-V image has booted, the Cortex-M33 cores are no longer the running
# architecture and openocd's default ARM target may fail to examine them. The
# boot ROM still starts in the OTP-selected architecture (ARM by default) and
# only switches after reading the image header, so an ARM attach normally still
# wins the race to halt. If it does not, fall back to attaching to the Hazard3
# cores instead.
if ! ./tools/flash.sh "$ROLE" "$IMAGE@$LOAD"; then
    echo "==> ARM attach failed; retrying against the RISC-V cores"
    OOCD_CORES="rv0 rv1" ./tools/flash.sh "$ROLE" "$IMAGE@$LOAD"
fi

LOG=logs/smoke-riscv.log
mkdir -p logs
: > "$LOG"

# Capture first, reset second. A kernel prints its boot log exactly once, so a
# console opened after the reset misses it entirely -- and the failure looks
# identical to a kernel that never booted. (The reference firmware hid this
# ordering bug by re-running its diagnostic every 5 s; Linux will not.)
echo "==> opening console"
python3 tools/console.py capture "$ROLE" --out "$LOG" --quiet &
CAPTURE_PID=$!
trap 'kill $CAPTURE_PID 2>/dev/null || true' EXIT

# Give pyserial time to open the port before anything is sent to it.
until [ -s "$LOG" ] || ! kill -0 $CAPTURE_PID 2>/dev/null; do sleep 0.2; done

echo "==> resetting $ROLE"
# Not reset.sh: that drives the Cortex-M33 targets, and a RISC-V image leaves
# them powered down. rescue() restarts the chip from the boot ROM instead, which
# is the only route in once the Hazard3 cores have taken over.
rescue "$ROLE"
oocd "$ROLE" "init" "reset run" "exit" >/dev/null 2>&1 || true

echo "==> waiting for the kernel"
DEADLINE=$(( $(date +%s) + 120 ))
PROMPT='~ #|/ #|login:|Welcome to Buildroot'
while [ "$(date +%s)" -lt "$DEADLINE" ]; do
    grep -qaE "$PROMPT" "$LOG" && break
    sleep 1
done

# One capture, two questions, because they fail differently and the difference
# is the whole diagnosis. "Linux version" means the bootloader brought up PSRAM,
# copied the kernel and handed over -- the hard part. A prompt after it means
# userspace came up too. The first without the second points at the initramfs
# or the console, not at the board.
kill $CAPTURE_PID 2>/dev/null || true
wait $CAPTURE_PID 2>/dev/null || true

if ! grep -qa 'Linux version' "$LOG"; then
    echo
    tail -30 "$LOG" >&2
    die "no kernel banner -- bootloader, PSRAM or QMI (log: $LOG)"
fi
echo "    kernel:  $(grep -am1 'Linux version' "$LOG" | sed 's/^\[ *[0-9.]*\] *//')"

if ! grep -qaE "$PROMPT" "$LOG"; then
    echo
    tail -30 "$LOG" >&2
    die "kernel booted but no shell -- initramfs or console (log: $LOG)"
fi

echo "PASS  Phase 1: RISC-V Linux booted to a shell on the $ROLE half"
