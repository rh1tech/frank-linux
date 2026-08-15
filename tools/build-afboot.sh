#!/usr/bin/env bash
#
# build-afboot.sh [--clean] - build the slave's bootloader.
#
# SDK resolution mirrors the FRANK firmware's sdk_env.sh: an exported
# PICO_SDK_PATH wins only if it actually contains an SDK. A stale export left
# pointing at a moved directory is a common and confusing way to fail, and there
# is one on this machine.

set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"

SRC="$REPO/firmware/afboot-rp2350"
BLD="$REPO/build/afboot"
[ "${1:-}" = "--clean" ] && rm -rf "$BLD"

: "${CPU_SPEED:=252}"
: "${PSRAM_SPEED:=133}"
: "${FLASH_SPEED:=66}"

if [ ! -f "${PICO_SDK_PATH:-}/pico_sdk_init.cmake" ]; then
    for c in "$HOME/pico/pico-sdk" "$HOME/pico-sdk" "$HOME/Documents/pico/pico-sdk" "$HOME/.pico-sdk"; do
        [ -f "$c/pico_sdk_init.cmake" ] && export PICO_SDK_PATH="$c" && break
    done
fi
[ -f "${PICO_SDK_PATH:-}/pico_sdk_init.cmake" ] || { echo "Pico SDK not found" >&2; exit 1; }
echo "Pico SDK: $PICO_SDK_PATH"

mkdir -p "$BLD"
cmake -S "$SRC" -B "$BLD" -DPICO_PLATFORM=rp2350 \
      -DCPU_SPEED="$CPU_SPEED" -DPSRAM_SPEED="$PSRAM_SPEED" \
      -DFLASH_SPEED="$FLASH_SPEED" >/dev/null
cmake --build "$BLD" -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" 2>&1 \
    | grep -vE "^\[|^gmake" || true
ls -l "$BLD/afboot-rp2350.elf"
