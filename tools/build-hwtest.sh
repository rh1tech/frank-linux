#!/usr/bin/env bash
#
# build-hwtest.sh <name> [--clean] - build one Phase 2 hardware test.
#
# These run on the master half, because that is where the UART console reaches
# a probe. The exclusive-monitor question is a property of the core and the bus
# fabric, which is identical on both halves, so measuring it on the master
# answers it for the slave where Linux will actually run. Bandwidth is not --
# that differs between the halves and is reported per half.

set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"

NAME="${1:?usage: build-hwtest.sh <name> [--clean]}"
SRC="$REPO/hwtests/$NAME"
BLD="$REPO/build/hwtest-$NAME"
[ -d "$SRC" ] || { echo "no such hwtest: $SRC" >&2; exit 1; }
[ "${2:-}" = "--clean" ] && rm -rf "$BLD"

: "${CPU_SPEED:=252}"
: "${PSRAM_SPEED:=133}"

# The Pico SDK is located the same way the FRANK firmware does it: an exported
# PICO_SDK_PATH wins only if it actually contains an SDK, because a stale export
# pointing at a moved directory is a confusing way to fail.
if [ ! -f "${PICO_SDK_PATH:-}/pico_sdk_init.cmake" ]; then
    for c in "$HOME/pico/pico-sdk" "$HOME/pico-sdk" "$HOME/Documents/pico/pico-sdk" "$HOME/.pico-sdk"; do
        [ -f "$c/pico_sdk_init.cmake" ] && export PICO_SDK_PATH="$c" && break
    done
fi
[ -f "${PICO_SDK_PATH:-}/pico_sdk_init.cmake" ] || { echo "Pico SDK not found" >&2; exit 1; }

mkdir -p "$BLD"
cmake -S "$SRC" -B "$BLD" -DPICO_PLATFORM=rp2350 \
      -DCPU_SPEED="$CPU_SPEED" -DPSRAM_SPEED="$PSRAM_SPEED" >/dev/null
cmake --build "$BLD" -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" 2>&1 | grep -vE "^\[|^gmake" || true
ls -l "$BLD"/hwtest-"$NAME".elf
