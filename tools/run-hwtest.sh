#!/usr/bin/env bash
#
# run-hwtest.sh <name> [role] - flash a Phase 2 hardware test and collect it.
#
# Flash halted, open the console, then reset. A test prints its results once, so
# a console opened after the reset misses them and the failure is
# indistinguishable from a test that never ran.
#
# Always prints the log, pass or fail. A hardware test that hangs is itself a
# finding -- an exclusive retry loop that never terminates is exactly the
# outcome we are looking for -- so the partial output is the evidence, not
# noise to be swallowed.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
cd "$REPO"

# shellcheck source=probe.sh
source "$HERE/probe.sh"

NAME="${1:?usage: run-hwtest.sh <name> [master|slave]}"
ROLE="${2:-master}"
TIMEOUT="${HWTEST_TIMEOUT:-90}"

ELF="build/hwtest-$NAME/hwtest-$NAME.elf"
LOG="logs/hwtest-$NAME.log"
[ -f "$ELF" ] || die "no $ELF -- run tools/build-hwtest.sh $NAME"

mkdir -p logs
: > "$LOG"

./tools/flash.sh "$ROLE" "$ELF" --halt >/dev/null

python3 tools/console.py capture "$ROLE" --out "$LOG" --quiet &
CAPTURE_PID=$!
cleanup() { kill "$CAPTURE_PID" 2>/dev/null || true; }
trap cleanup EXIT

# Wait for the console to actually open, but give up if it died -- otherwise a
# pyserial failure turns into an unbounded spin with no output at all.
for _ in $(seq 1 50); do
    [ -s "$LOG" ] && break
    kill -0 "$CAPTURE_PID" 2>/dev/null || die "console capture died; see $LOG"
    sleep 0.2
done

./tools/reset.sh "$ROLE" >/dev/null

DEADLINE=$(( $(date +%s) + TIMEOUT ))
DONE=0
while [ "$(date +%s)" -lt "$DEADLINE" ]; do
    if grep -q "=== done ===" "$LOG"; then DONE=1; break; fi
    sleep 1
done

cleanup
sleep 0.3

echo
sed 's/^\[ *[0-9.]*\] //' "$LOG" | grep -vE '^=====|^$|waiting for console' || true
echo

if [ "$DONE" = 1 ]; then
    echo "PASS  hwtest $NAME completed on the $ROLE half (log: $LOG)"
else
    echo "FAIL  hwtest $NAME did not finish within ${TIMEOUT}s (log: $LOG)"
    echo "      A test that hangs is a result: check which RESULT line is missing."
    exit 1
fi
