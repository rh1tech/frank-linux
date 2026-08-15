#!/usr/bin/env bash
#
# qemu-arm.sh [--shell] - boot the ARM NOMMU kernel under QEMU.
#
# The Phase 3 gate. No hardware, so this is also the CI target: kernel changes
# stay bisectable without the board in the loop, and a failure here is never
# "maybe the probe came loose".
#
# --shell drops you into an interactive session instead of asserting and exiting.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
cd "$REPO"

MACHINE="${QEMU_MACHINE:-mps2-an385}"
IMAGES=build/qemu-armv7m
LOG=logs/qemu-arm.log
TIMEOUT="${QEMU_TIMEOUT:-90}"

INTERACTIVE=0
[ "${1:-}" = "--shell" ] && INTERACTIVE=1

command -v qemu-system-arm >/dev/null || {
    echo "qemu-system-arm not found" >&2; exit 1; }

# QEMU loads an ELF directly and takes its entry point and load addresses from
# the headers, which saves needing a boot wrapper to place the image and set the
# vector table. Prefer it; fall back to whatever Buildroot produced.
KERNEL=""
for cand in "$IMAGES/vmlinux" "$IMAGES/zImage" "$IMAGES/Image" "$IMAGES/xipImage"; do
    [ -f "$cand" ] && { KERNEL="$cand"; break; }
done
[ -n "$KERNEL" ] || {
    echo "no kernel image in $IMAGES -- run 'make qemu-arm' first" >&2
    ls -l "$IMAGES" 2>/dev/null || true
    exit 1
}

mkdir -p logs
echo "==> $MACHINE  <-  $KERNEL"

# shellcheck disable=SC2054  # the comma is inside a QEMU option value, not a separator
QEMU_ARGS=(
    -M "$MACHINE"
    -kernel "$KERNEL"
    -nographic
    -monitor none
    -semihosting-config enable=on,target=native
)

if [ "$INTERACTIVE" = 1 ]; then
    echo "    (interactive; quit with Ctrl-A X)"
    exec qemu-system-arm "${QEMU_ARGS[@]}"
fi

: > "$LOG"
# QEMU with -nographic wires the serial console to stdio, so the boot log simply
# arrives on our stdout. No console.py here: there is no tty and no probe, which
# is the point of this target.
( qemu-system-arm "${QEMU_ARGS[@]}" < /dev/null > "$LOG" 2>&1 & echo $! > /tmp/qemu-arm.pid ) || true
QPID="$(cat /tmp/qemu-arm.pid)"
cleanup() { kill "$QPID" 2>/dev/null || true; }
trap cleanup EXIT

PROMPT='~ #|/ #|login:|Welcome to Buildroot'
DEADLINE=$(( $(date +%s) + TIMEOUT ))
while [ "$(date +%s)" -lt "$DEADLINE" ]; do
    grep -qaE "$PROMPT" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 1
done
cleanup
sleep 0.3

echo
tail -40 "$LOG"
echo

# Same two questions as the hardware gate, and they fail differently: a banner
# without a prompt is userspace or the initramfs, no banner at all is the image
# or the machine.
if ! grep -qa 'Linux version' "$LOG"; then
    echo "FAIL  no kernel banner -- image, load address or machine (log: $LOG)" >&2
    exit 1
fi
if ! grep -qaE "$PROMPT" "$LOG"; then
    echo "FAIL  kernel booted but no shell -- initramfs, FDPIC loader or console (log: $LOG)" >&2
    exit 1
fi
echo "PASS  Phase 3: ARM NOMMU kernel booted to a shell under $MACHINE"
