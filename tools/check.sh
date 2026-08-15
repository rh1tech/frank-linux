#!/usr/bin/env bash
#
# check.sh - the Phase 0 gate.
#
# Validates the harness against firmware already known to work, so that when it
# later judges our own code the verdict can be trusted. Nothing here tests the
# Linux port; it tests the instruments.
#
#   1. bench    every probe and the capture card resolve by USB serial / name
#   2. fonts    Protea's font sheets load with the right geometry, right way up
#   3. decoder  a synthetic frame round-trips through the decoder exactly
#   4. flash    both halves programmed over SWD, each verified to be the right
#               chip before writing
#   5. serial   the master reports LINK OK on its console
#   6. video    a frame is captured from HDMI
#
# Usage: tools/check.sh [--no-build]

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
cd "$REPO"

BUILD=1
[ "${1:-}" = "--no-build" ] && BUILD=0

FAILED=0
step() { printf '\n\033[1m== %s ==\033[0m\n' "$*"; }
fail() { echo "FAIL  $*"; FAILED=1; }

step "1. bench instruments"
python3 tools/devices.py check || fail "bench instruments"

step "2. Protea fonts"
for f in vga ega cga; do
    python3 tools/fontgen.py selftest "$f" || fail "font $f"
done

step "3. screen decoder round-trip"
python3 tools/test_screen.py || fail "screen decoder"

if [ "$BUILD" = 1 ]; then
    step "4. build reference firmware"
    ./tools/build-reffw.sh >/dev/null 2>&1 || fail "reference firmware build"
fi

ELF_DIR="build/reffw-hid1"
if [ ! -f "$ELF_DIR/master/frank-core2-master.elf" ]; then
    fail "no reference firmware at $ELF_DIR -- run without --no-build"
else
    step "5. flash both halves"
    # Slave first: the master's idle loop re-probes every 5 s and re-runs its
    # diagnostic when the slave answers, so a slave that comes up into an
    # already-running master converges without waiting.
    ./tools/flash.sh slave  "$ELF_DIR/slave/frank-core2-slave.elf"   || fail "flash slave"
    ./tools/flash.sh master "$ELF_DIR/master/frank-core2-master.elf" || fail "flash master"
    ./tools/reset.sh both >/dev/null

    step "6. master console reports LINK OK"
    python3 tools/console.py wait master 'LINK OK' --timeout 45 --quiet \
        || fail "master console did not report LINK OK"

    step "7. HDMI capture"
    # Only that a frame arrives. The reference firmware draws with its own 6x8
    # font, not Protea's, so the text decoder cannot read this screen -- and
    # would rightly refuse to. The decoder is covered by step 3 instead.
    if python3 - <<'EOF'
import sys, os
sys.path.insert(0, "tools")
import screen
raw = screen.grab()
lo, hi = min(raw), max(raw)
os.makedirs("logs", exist_ok=True)
screen.save_png(raw, "logs/screen.png")
# A disconnected or unlocked capture is a uniform field. Real video has range.
print(f"    frame {len(raw)} bytes, luma {lo}..{hi} -> logs/screen.png")
sys.exit(0 if hi - lo > 40 else 1)
EOF
    then :; else fail "HDMI capture is blank or the card is not locked"; fi
fi

printf '\n'
if [ "$FAILED" = 0 ]; then
    echo "PASS  harness validated against known-good firmware"
else
    echo "FAIL  harness not validated"
fi
exit "$FAILED"
