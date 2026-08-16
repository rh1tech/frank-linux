#!/usr/bin/env bash
#
# check-atomics.sh - find binaries that will livelock on this hardware.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# LDREX/STREX need an exclusive monitor, and this chip has none covering the
# memory the system runs from (F6). STREX there does not fail in a way that can
# be retried -- it never succeeds -- so a compare-exchange retry loop spins
# forever. One such instruction on one rarely-taken path is enough to hang a
# program permanently, with no message and nothing in the kernel log.
#
# That is a bad thing to discover on the board. It cost most of a day once
# (F29): the machine mounted its root, exec'd init, printed nothing, and the
# only way to find out why was to halt the core over SWD and disassemble the
# address the program counter had stopped at.
#
# It is also easy to reintroduce, because every build system asks the wrong
# question. uClibc and glib both decide whether to use inline atomics by
# checking whether a test program *compiles and links* -- and it does. The code
# is valid; it simply never terminates when run. Neither meson nor autoconf runs
# test programs when cross-compiling, so neither can find out.
#
# So ask the binaries. This is the only check that sees the truth.
#
# Usage:
#   check-atomics.sh                  scan the built target tree
#   check-atomics.sh path [path...]   scan specific files or trees

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE/.." || exit 1

# The scan runs in the build container: the cross objdump lives there, and the
# host's does not know this architecture.
docker run --rm -v frank-linux-out:/out frank-linux-build sh -s -- \
    "${@:-/out/core2-slave/target}" <<'EOF'
set -u

OD=$(ls /out/core2-slave/host/bin/*-objdump 2>/dev/null | head -1)
[ -n "$OD" ] || { echo "no cross objdump -- has the toolchain been built?" >&2; exit 2; }

report=$(mktemp)
trap 'rm -f "$report"' EXIT

for root in "$@"; do
    [ -e "$root" ] || { echo "no such path: $root" >&2; exit 2; }

    # -type f, so the forty symlinks to busybox are counted once rather than
    # burying the signal under the same binary reported repeatedly.
    find "$root" -type f 2>/dev/null | while read -r f; do
        case "$(LC_ALL=C file -b "$f" 2>/dev/null)" in
            *ELF*) ;;
            *) continue ;;
        esac
        n=$("$OD" -d "$f" 2>/dev/null |
            grep -ciE "[[:space:]](ldrex|strex)[bhd]?[[:space:]]")
        printf '%s\t%s\n' "${n:-0}" "${f#"$root"/}" >> "$report"
    done
done

total=$(wc -l < "$report" | tr -d ' ')
bad=$(awk -F'\t' '$1 != 0' "$report" | wc -l | tr -d ' ')

awk -F'\t' '$1 != 0 { printf "FAIL  %-44s %s exclusive instruction(s)\n", $2, $1 }' "$report"

echo
if [ "$bad" != 0 ]; then
    echo "FAIL  atomics: $bad of $total binaries will livelock on this hardware"
    echo "      See br-external/patches/uclibc/0002 and libglib2/0001 for the"
    echo "      two ways this has been dealt with: route the library's one"
    echo "      compare-exchange macro through the kernel, or tell the package"
    echo "      its inline atomics are not lock-free so it uses mutexes."
    exit 1
fi
echo "PASS  atomics: $total binaries, no LDREX/STREX"
EOF
