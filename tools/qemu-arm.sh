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

KERNEL="$IMAGES/vmlinux"
DTB="$IMAGES/mps2-an385.dtb"
WRAPPER="$IMAGES/wrapper.elf"

[ -f "$KERNEL" ] || {
    echo "no vmlinux in $IMAGES -- run 'make qemu-arm' first" >&2
    ls -l "$IMAGES" 2>/dev/null || true
    exit 1
}

# Build the boot wrapper if it is missing or stale.
#
# A Cortex-M has no jump-to-entry reset: it reads the initial SP and the reset
# handler from the vector table at address 0. The kernel links at 0x21008000
# with nothing at 0, so handing QEMU vmlinux alone starts execution at address 0
# and locks up instantly ("can't escalate 3 to HardFault", R15=00000000). The
# wrapper is the two words that have to be at 0, plus the ARM boot protocol.
if [ ! -f "$WRAPPER" ] || [ boot/qemu-armv7m/wrapper.S -nt "$WRAPPER" ]; then
    echo "==> building boot wrapper"
    arm-none-eabi-gcc -mcpu=cortex-m3 -mthumb -nostdlib -nostartfiles \
        -T boot/qemu-armv7m/wrapper.lds -o "$WRAPPER" boot/qemu-armv7m/wrapper.S
fi

mkdir -p logs
echo "==> $MACHINE  <-  $KERNEL"

# shellcheck disable=SC2054  # the comma is inside a QEMU option value, not a separator
# The wrapper is the -kernel, because it is what must sit at address 0. vmlinux
# and the DTB come in as additional loader devices: an ELF places itself from
# its program headers, and the DTB goes at 0x21000000, the 32 KiB of DRAM below
# the kernel's text, which is where the wrapper points r2.
QEMU_ARGS=(
    -M "$MACHINE"
    -kernel "$WRAPPER"
    -device "loader,file=$KERNEL"
    -nographic
    -monitor none
    -semihosting-config enable=on,target=native
)
[ -f "$DTB" ] && QEMU_ARGS+=(-device "loader,file=$DTB,addr=0x21000000,force-raw=on")

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
