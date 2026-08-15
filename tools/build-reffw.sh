#!/usr/bin/env bash
#
# build-reffw.sh [--clean] - build the FRANK Core 2 Proto bring-up firmware.
#
# This is not our firmware. It is the known-good target that validates the test
# harness: it exercises HDMI, the link, PSRAM, flash and both consoles, and its
# expected output is documented. Proving the harness against it means that when
# the harness later says our own code failed, that verdict can be trusted.
#
# Built with USB_HID=1 so both consoles move to UART (master UART0 on J2, slave
# UART1 on J4) and arrive through the probes' CDC bridges, which is the channel
# the harness reads. The default USB_HID=0 build puts them on the boards' own
# USB-C ports instead, which the harness cannot address by role.
#
# We invoke cmake directly rather than the upstream build.sh because we need one
# extra include path (firmware/compat) to work around a stale include there; see
# firmware/compat/frank_core2_board.h. Everything else mirrors master/build.sh.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"

REFFW="${REFFW:-$HOME/Documents/GitHub/frank-lab/frank_core2_proto/firmware}"
[ -d "$REFFW" ] || { echo "reference firmware not found: $REFFW" >&2; exit 1; }

COMPAT="$REPO/firmware/compat"
OUT="$REPO/build/reffw"

# Matched builds only. The link receiver's PIO loop must complete inside the
# transmitter's byte period, and each side derives that from its own system
# clock, so a mismatch gives a link that works one way and drops bytes the
# other. Both halves are built here from the same variables for that reason.
: "${CPU_SPEED:=252}"
: "${PSRAM_SPEED:=133}"
: "${FLASH_SPEED:=66}"

# USB_HID=1 (default) puts both consoles on UART, which is the channel the
# harness reads. USB_HID=0 moves them to the boards' own USB-C ports and is
# useful as a control: it is the only way to measure what the TinyUSB HID host
# task costs the master in memory bandwidth and link latency.
: "${USB_HID:=1}"
case "$USB_HID" in
    1|ON|on|yes|true) USB_HID_CMAKE=ON;  USB_HID_DEFINE="-DUSB_HID_ENABLED=1" ;;
    *)                USB_HID_CMAKE=OFF; USB_HID_DEFINE= ;;
esac
OUT="$OUT-hid$USB_HID"

CLEAN=0
[ "${1:-}" = "--clean" ] && CLEAN=1

# shellcheck source=/dev/null
source "$REFFW/sdk_env.sh"

for half in master slave; do
    src="$REFFW/$half"
    bld="$OUT/$half"
    [ "$CLEAN" = 1 ] && rm -rf "$bld"
    mkdir -p "$bld"

    echo "=== building $half (CPU ${CPU_SPEED} MHz, PSRAM ${PSRAM_SPEED} MHz, USB_HID=${USB_HID}) ==="
    # The compat include goes in via the CFLAGS environment variable, not
    # -DCMAKE_C_FLAGS. CMake seeds CMAKE_C_FLAGS from CMAKE_C_FLAGS_INIT *and*
    # $CFLAGS, but a -D on the command line pre-populates the cache entry and
    # the INIT flags are then dropped entirely -- which silently discards the
    # Pico SDK toolchain's -mcpu=cortex-m33 -mthumb and builds for ARM mode.
    # That fails deep in the SDK ("selected processor does not support mcrr",
    # "no SW_SPIN_LOCK_LOCK available for this platform") rather than anywhere
    # near the flag that caused it.
    # -DUSB_HID_ENABLED=1 is ours to add, and it is not optional. Both halves
    # guard on the macro in C (`#if !defined(USB_HID_ENABLED)` around the
    # pico/stdio_usb.h include, `#ifdef USB_HID_ENABLED` around HID init) but
    # neither CMakeLists ever puts it in target_compile_definitions -- it only
    # exists as a CMake option controlling pico_enable_stdio_usb(). So a
    # USB_HID=ON build switches the CDC stdio library off in CMake while the
    # source still compiles as if it were a CDC build: the slave fails outright
    # on the missing header, and the master compiles but never initialises the
    # HID host and writes its console to a USB CDC that is not there. Defining
    # the macro here is what makes the option mean what it says.
    CFLAGS="-I$COMPAT $USB_HID_DEFINE" \
    CXXFLAGS="-I$COMPAT $USB_HID_DEFINE" \
    cmake -S "$src" -B "$bld" \
        -DPICO_PLATFORM=rp2350 \
        -DCPU_SPEED="$CPU_SPEED" \
        -DPSRAM_SPEED="$PSRAM_SPEED" \
        -DFLASH_SPEED="$FLASH_SPEED" \
        -DUSB_HID_ENABLED=$USB_HID_CMAKE \
        >/dev/null
    cmake --build "$bld" -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" >/dev/null
    echo "    $(ls "$bld"/frank-core2-$half.elf)"
done

echo
echo "Reference firmware built:"
echo "  $OUT/master/frank-core2-master.elf"
echo "  $OUT/slave/frank-core2-slave.elf"
