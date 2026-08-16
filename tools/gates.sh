#!/usr/bin/env bash
#
# gates.sh - run every hardware gate, in one pass, without touching the tree.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The gates are separate scripts because each one is a separate claim, and that
# stays true. What this adds is a way to run all of them from a single command
# so that nothing edits tools/ or build/ while they are in flight.
#
# That is not tidiness. Bash reads a script as a byte offset into an open file,
# so editing one that is running makes the shell resume at that offset in a
# file that has moved underneath it -- and a build's final step copies fresh
# images into build/core2-slave/ while a gate may be flashing out of it. Both
# have already happened here, both corrupted a run, and both produced errors
# that read as board faults (docs/hw-findings.md F27).
#
# Each gate reflashes both halves, so they are slow and deliberately sequential:
# there is one board.
#
# Usage:
#   gates.sh              every gate
#   gates.sh mpu blk      only those, in the order given
#   KEEP_GOING=1 gates.sh continue past a failure instead of stopping

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
cd "$REPO" || exit 1

# Ordered cheapest-evidence-first, so a fundamental breakage shows up in the
# first few minutes rather than the last: if the machine will not boot with an
# MPU, nothing after it means anything.
ALL=(mpu blk console vi nano mc)

GATES=("$@")
[ ${#GATES[@]} -eq 0 ] && GATES=("${ALL[@]}")

for g in "${GATES[@]}"; do
    [ -x "tools/test-$g.sh" ] || { echo "no tools/test-$g.sh" >&2; exit 1; }
done

mkdir -p logs
SUMMARY=logs/gates.log
: > "$SUMMARY"

passed=() failed=()
start_all=$SECONDS

for g in "${GATES[@]}"; do
    echo
    echo "================================================================"
    echo "  $g"
    echo "================================================================"
    start=$SECONDS
    # Output goes to a per-gate log as well as the terminal, so a failure can be
    # read afterwards without spending four more minutes of board time on it.
    # PIPESTATUS[0] rather than $?: pipefail is on, so $? would give the same
    # answer here, but only because tee happens to succeed. Reading the gate's
    # own status says what is meant and does not depend on a shell option two
    # dozen lines away.
    "tools/test-$g.sh" 2>&1 | tee "logs/gate-$g.log"
    rc=${PIPESTATUS[0]}
    took=$((SECONDS - start))

    if [ "$rc" -eq 0 ]; then
        passed+=("$g")
        printf 'PASS  %-8s %3ds\n' "$g" "$took" | tee -a "$SUMMARY"
    else
        failed+=("$g")
        printf 'FAIL  %-8s %3ds  (logs/gate-%s.log)\n' "$g" "$took" "$g" \
            | tee -a "$SUMMARY"
        [ -n "${KEEP_GOING:-}" ] || break
    fi
done

echo
echo "================================================================"
printf '%d passed, %d failed, %ds total\n' \
    "${#passed[@]}" "${#failed[@]}" "$((SECONDS - start_all))"
if [ ${#failed[@]} -ne 0 ]; then
    printf 'failed: %s\n' "${failed[*]}"
    # Name what was never reached, rather than letting a short list of passes
    # look like a short list of gates.
    skipped=()
    for g in "${GATES[@]}"; do
        case " ${passed[*]} ${failed[*]} " in *" $g "*) ;; *) skipped+=("$g");; esac
    done
    [ ${#skipped[@]} -ne 0 ] && printf 'not run: %s\n' "${skipped[*]}"
    exit 1
fi
echo "all gates passed"
