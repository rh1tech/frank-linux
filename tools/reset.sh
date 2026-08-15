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

reset_one() {
    local role="$1"
    oocd "$role" "init" "reset $MODE" "exit" >/dev/null
    echo "==> $role reset ($MODE)"
}

case "$TARGET" in
    master|slave) reset_one "$TARGET" ;;
    both) reset_one slave; reset_one master ;;
    *) usage ;;
esac
