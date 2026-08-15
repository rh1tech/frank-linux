#!/usr/bin/env bash
#
# flash.sh <master|slave> <image.elf> [--halt]
#
# Program one board half over SWD and leave it running.
#
# Always verifies which half it is talking to first. Both chips are RP2350 and
# answer identically over SWD, so if the probes get swapped between J1 and J3
# openocd will cheerfully erase the wrong chip and report success.
#
# --halt leaves the target halted instead of running. Do not use it while
# asserting on HDMI: halting stops core 1, the HSTX scanout dies, and the
# capture card shows a "no signal" colour-bar pattern.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=probe.sh
source "$HERE/probe.sh"

usage() { echo "usage: $(basename "$0") <master|slave> <image.elf> [--halt]" >&2; exit 2; }

[ $# -ge 2 ] || usage
ROLE="$1"; IMAGE="$2"; shift 2

# Split an optional @0xADDR suffix off the image path.
LOAD_ADDR=""
case "$IMAGE" in
    *@0x*|*@[0-9]*) LOAD_ADDR="${IMAGE##*@}"; IMAGE="${IMAGE%@*}" ;;
esac

RUN_AFTER=run
while [ $# -gt 0 ]; do
    case "$1" in
        --halt) RUN_AFTER=halt ;;
        *) usage ;;
    esac
    shift
done

[ -f "$IMAGE" ] || die "no such image: $IMAGE"

# openocd's `program` wants an ELF, HEX or raw binary with a load address. UF2
# is a USB-bootloader container: it carries its own addressing that openocd
# does not parse, so pointing this at a .uf2 would flash the container header
# as if it were code. The pico-sdk emits the .elf alongside every .uf2.
case "$IMAGE" in
    *.uf2) die "flash.sh programs over SWD and cannot read UF2.
       Use the .elf the pico-sdk built next to it, or picotool for USB." ;;
esac

# A raw binary carries no addresses, so it needs one: pass it as file@0xADDR.
# Refusing to guess matters here -- flash XIP starts at 0x10000000 and PSRAM at
# 0x11000000, and defaulting to either would silently write to the wrong one.
if [ -n "$LOAD_ADDR" ]; then
    WRITE_CMD="flash write_image erase $IMAGE $LOAD_ADDR bin"
    VERIFY_CMD="verify_image $IMAGE $LOAD_ADDR bin"
else
    case "$IMAGE" in
        *.elf|*.hex)
            WRITE_CMD="flash write_image erase $IMAGE"
            VERIFY_CMD="verify_image $IMAGE" ;;
        *) die "$IMAGE has no load address. Raw images must be given as
       file@0xADDR (flash XIP is 0x10000000)." ;;
    esac
fi

# Rescue first, every time.
#
# openocd's `program` does a `reset init`, which lets whatever is already in
# flash start running -- and a bad image gets to reconfigure the QMI before the
# debugger can probe it. The failure is spectacular and confusing: "QSPI Flash
# id = 0x0c20f7 not recognised", "clearing lockup after double fault",
# "Failed to init Arm core 0 before ROM call". The board is fine; it is simply
# executing something that broke XIP.
#
# rescue_reset stops both cores in the boot ROM before any image header is read,
# so nothing on the flash gets a chance to run. It costs about a second and it
# makes flashing work regardless of what is currently installed -- which during
# a kernel bring-up is most of the time.
# Identify first, then rescue. assert_half has to let the ROM run to latch
# PACKAGE_SEL, and rescue_reset stops it before that happens -- doing them the
# other way round makes every half look like a QFN-80.
echo "==> verifying $ROLE half"
assert_half "$ROLE"

rescue "$ROLE"
ensure_arm "$ROLE"

echo "==> flashing $ROLE <- $IMAGE${LOAD_ADDR:+ @ $LOAD_ADDR}"
# Read the transcript, not the exit status, in both directions.
#
# openocd exits 0 on some verify failures, so success has to be confirmed in the
# log. It also exits non-zero here on a *successful* flash of a RISC-V image:
# `program ... reset` writes and verifies fine, then the trailing reset boots
# the new image, the chip switches to the Hazard3 cores, and re-attaching to the
# now-powered-down M33 fails. Treating that as a flash failure would discard a
# perfectly good write.
# Explicit write and verify rather than openocd's `program`.
#
# `program` performs a `reset init` first, which boots whatever is already in
# flash -- undoing the rescue above and handing control back to the very image
# that may be breaking XIP. Halting the already-rescued cores and writing from
# there never lets a stale image run at all.
LOG="$(oocd "$ROLE" "init" "halt" "$WRITE_CMD" "$VERIFY_CMD" "exit")" || true

# "verified N bytes" is the success line from verify_image; "Verified OK" is
# `program`'s. Accept either, and read the transcript rather than the exit
# status: openocd exits non-zero after a *successful* flash of a RISC-V image,
# because the trailing reset switches the chip to the Hazard3 cores and the M33
# targets it then tries to re-attach to are powered down.
if ! grep -qE "Verified OK|verified [0-9]+ bytes" <<<"$LOG"; then
    echo "$LOG" >&2
    die "$ROLE: image did not verify"
fi
grep -E "wrote [0-9]+ bytes|Verified OK|verified [0-9]+ bytes" <<<"$LOG" | sed 's/^/    /'

if [ "$RUN_AFTER" = halt ]; then
    echo "==> $ROLE halted"
else
    oocd "$ROLE" "init" "reset run" "exit" >/dev/null 2>&1 || true
    echo "==> $ROLE running"
fi
