#!/usr/bin/env bash
#
# reset.sh <master|slave|both> [--halt]
#
# Reset a half without reflashing it. Two probes mean each half resets
# independently, which is what makes the board's known "master cannot reset the
# slave" bug irrelevant during development (the GPIO43 net does not exist -- see
# frank_core2_proto/firmware/README.md).
#
# `both` resets the slave first. The master's idle loop re-probes for the slave
# every 5 s and re-runs its diagnostic when it answers, so bringing the slave up
# into an already-running master is the ordering that converges without waiting.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=probe.sh
source "$HERE/probe.sh"

usage() { echo "usage: $(basename "$0") <master|slave|both> [--halt]" >&2; exit 2; }

[ $# -ge 1 ] || usage
TARGET="$1"; shift

MODE=run
while [ $# -gt 0 ]; do
    case "$1" in
        --halt) MODE=halt ;;
        *) usage ;;
    esac
    shift
done

# Reset, then make sure the half came back.
#
# A plain `reset run` sometimes leaves the slave unreachable: the debug port
# still answers DPIDR but no memory can be read, which is the F26 signature --
# the QSPI flash left in a state where XIP returns nothing, so neither core can
# fetch. Resetting the CPU while the QMI is mid-transaction is one way in.
#
# flash.sh has always handled this, because it calls rescue/ensure_arm before
# writing. reset.sh did not, so a reset that wedged the board left it wedged,
# and the only way out was a reflash -- eight minutes to recover from a
# two-second operation. Worse, the wedge looks like whatever was running last,
# so it kept getting blamed on the software under test.
#
# ensure_arm() rescues and re-checks. RESCUE_RESTART resets everything except
# the DP and RP_AP, which is enough to unstick the flash chip without a power
# cycle.
reset_one() {
    local role="$1"
    oocd "$role" "init" "reset $MODE" "exit" >/dev/null 2>&1 || true

    if ! arm_reachable "$role"; then
        echo "==> $role did not come back from reset; rescuing" >&2
        rescue "$role"
        arm_reachable "$role" || die "$role: unreachable after rescue.
       Hold BOOTSEL on that half and power-cycle to recover."
        # The rescue leaves both cores stopped in the boot ROM, so the reset
        # that was asked for still has to happen.
        oocd "$role" "init" "reset $MODE" "exit" >/dev/null 2>&1 || true
    fi

    echo "==> $role reset ($MODE)"
}

case "$TARGET" in
    master|slave) reset_one "$TARGET" ;;
    both) reset_one slave; reset_one master ;;
    *) usage ;;
esac
