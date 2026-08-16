#!/usr/bin/env bash
#
# test-mpu.sh - is the MPU on, and does it actually keep userspace out?
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Two separate claims, because the first does not imply the second. A kernel can
# report a perfectly initialised MPU and still have mapped everything to
# everyone; on NOMMU without one, a user process can read and write kernel
# memory freely, and that is the thing worth fixing.
#
#   1. the kernel says it programmed the MPU        (boot log)
#   2. MPU_CTRL really has ENABLE set               (SWD, not the kernel's word)
#   3. kernel text is read-only and data is XN      (SWD, region permissions)
#   4. a user process reading kernel memory dies    (mputest, on the target)
#
# The address in (3) is not guessed: it is read out of the region the kernel
# programmed, so the test cannot drift away from the layout it is checking.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
cd "$REPO"

# shellcheck source=probe.sh
source "$HERE/probe.sh"

LOG=logs/mpu-test.log

# ARMv8-M MPU, from the ARMv8-M Architecture Reference Manual.
MPU_TYPE=0xe000ed90
MPU_CTRL=0xe000ed94
MPU_RNR=0xe000ed98
MPU_RBAR=0xe000ed9c
MPU_RLAR=0xe000eda0

echo "==> flashing the slave and booting Linux"
SLAVE_TIMEOUT=45 ./tools/flash-slave.sh >/dev/null || die "the slave did not boot Linux"
cp logs/slave-boot.log "$LOG"

if ! grep -qa "PMSAv8 Compliant MPU" "$LOG"; then
    die "the kernel did not report an MPU (log: $LOG)"
fi
grep -a "PMSAv8 Compliant MPU" "$LOG" | sed 's/^/    /'

echo "==> reading the MPU registers over SWD"
# The kernel's own log is not evidence that the hardware agrees with it.
regs="$(oocd slave "init" "halt" "mdw $MPU_TYPE 1" "mdw $MPU_CTRL 1" "exit" 2>/dev/null)"
mtype="$(sed -n "s/^$MPU_TYPE: *//p" <<<"$regs" | tr -d '\r' | awk '{print $1}')"
mctrl="$(sed -n "s/^$MPU_CTRL: *//p" <<<"$regs" | tr -d '\r' | awk '{print $1}')"
[ -n "$mtype" ] && [ -n "$mctrl" ] || die "could not read the MPU registers"
regions=$(( (0x$mtype >> 8) & 0xff ))
echo "    MPU_TYPE=0x$mtype ($regions regions), MPU_CTRL=0x$mctrl"

if [ $(( 0x$mctrl & 1 )) -ne 1 ]; then
    die "MPU_CTRL.ENABLE is clear -- the MPU is not on"
fi

# Find the kernel's own region: the one denying PL0 (AP=00 in RBAR[2:1]) that is
# enabled. Everything else is mapped PL0-readable.
echo "==> locating the privileged-only region"
kaddr=""
for n in $(seq 0 $((regions - 1))); do
    out="$(oocd slave "init" "halt" "mww $MPU_RNR $n" \
                "mdw $MPU_RBAR 1" "mdw $MPU_RLAR 1" "exit" 2>/dev/null)"
    rbar="$(sed -n "s/^$MPU_RBAR: *//p" <<<"$out" | tr -d '\r' | awk '{print $1}')"
    rlar="$(sed -n "s/^$MPU_RLAR: *//p" <<<"$out" | tr -d '\r' | awk '{print $1}')"
    [ -n "$rbar" ] && [ -n "$rlar" ] || continue
    [ $(( 0x$rlar & 1 )) -eq 1 ] || continue          # region disabled
    [ $(( (0x$rbar >> 1) & 3 )) -eq 0 ] || continue   # AP != PL1-only
    base=$(( 0x$rbar & 0xffffffe0 ))
    limit=$(( 0x$rlar & 0xffffffe0 ))
    # Aim at the middle, so the test is not sitting on a boundary.
    kaddr=$(printf "0x%08x" $(( base + (limit - base) / 2 )))
    printf "    region %d: 0x%08x..0x%08x is PL1-only; probing %s\n" \
           "$n" "$base" "$limit" "$kaddr"
    break
done
[ -n "$kaddr" ] || die "no privileged-only region found -- the kernel is not protected"

echo "==> checking the kernel's own text is read-only"
# STRICT_KERNEL_RWX splits the kernel region in two. Assert on the region
# registers rather than the boot message: the message is the kernel's opinion,
# these are what the hardware will actually enforce.
#
# RBAR[2:1] is AP -- 2 means PL1 read-only, PL0 no access -- and RBAR[0] is XN.
ro_exec=0
rw_xn=0
for n in $(seq 0 $((regions - 1))); do
    out="$(oocd slave "init" "halt" "mww $MPU_RNR $n" \
                "mdw $MPU_RBAR 1" "mdw $MPU_RLAR 1" "exit" 2>/dev/null)"
    rbar="$(sed -n "s/^$MPU_RBAR: *//p" <<<"$out" | tr -d '\r' | awk '{print $1}')"
    rlar="$(sed -n "s/^$MPU_RLAR: *//p" <<<"$out" | tr -d '\r' | awk '{print $1}')"
    [ -n "$rbar" ] && [ -n "$rlar" ] || continue
    [ $(( 0x$rlar & 1 )) -eq 1 ] || continue
    ap=$(( (0x$rbar >> 1) & 3 ))
    xn=$(( 0x$rbar & 1 ))
    if [ "$ap" -eq 2 ] && [ "$xn" -eq 0 ]; then
        ro_exec=1
        printf "    region %d: 0x%08x is PL1 read-only and executable (text)\n" \
               "$n" $(( 0x$rbar & 0xffffffe0 ))
    fi
    if [ "$ap" -eq 0 ] && [ "$xn" -eq 1 ]; then
        rw_xn=1
        printf "    region %d: 0x%08x is PL1 read-write and never executable (data)\n" \
               "$n" $(( 0x$rbar & 0xffffffe0 ))
    fi
done
[ "$ro_exec" -eq 1 ] || die "no read-only executable region: kernel text is writable"
[ "$rw_xn" -eq 1 ] || die "no writable non-executable region: kernel data is executable"

echo "==> asking a user process to read it"
python3 - "$LOG" "$kaddr" <<'PY'
import sys
sys.path.insert(0, "tools")
from slave_console import Console, reset_slave

log, kaddr = sys.argv[1], sys.argv[2]
# Halting for the register reads above stopped the kernel; start it again.
reset_slave()
c = Console()
c.drain(38)
c.send("", 2)
c.send("root", 3)
c.send(f"mputest {kaddr}", 5)
c.send("echo MPUTEST_EXIT=$?", 3)
open(log, "ab").write(c.buf)
print(c.text()[-900:])
PY

echo
if ! grep -qa "MPUTEST own .* OK" "$LOG"; then
    die "mputest could not read its own stack -- the test is broken, not the MPU"
fi
if grep -qa "MPUTEST RESULT UNPROTECTED" "$LOG"; then
    die "a user process read kernel memory: the MPU is on but not protecting it"
fi
if ! grep -qa "MPUTEST RESULT protected" "$LOG"; then
    die "mputest did not report a result (log: $LOG)"
fi
echo "PASS  MPU: PMSAv8 on $regions regions -- kernel text read-only, data"
echo "      non-executable, and userspace cannot reach either"
