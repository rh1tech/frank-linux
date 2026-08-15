#!/usr/bin/env bash
#
# probe.sh - shared SWD helpers. Source this; do not run it.
#
# Everything here addresses a board half by role ("master" / "slave"), never by
# probe. tools/devices.py maps role -> USB serial -> tty, so a replug or a
# different hub port changes nothing above this file.

set -euo pipefail

PROBE_SH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

: "${OPENOCD:=openocd}"
: "${ADAPTER_SPEED:=5000}"

# RP2350 SYSINFO, from the SDK's addressmap.h / sysinfo.h. Part of this file's
# interface: scripts that source it read these, so shellcheck cannot see the use.
# shellcheck disable=SC2034
SYSINFO_CHIP_ID=0x40000000
SYSINFO_PACKAGE_SEL=0x40000004

die() { echo "error: $*" >&2; exit 1; }

# PACKAGE_SEL bit 0: 1 = QFN-60 (RP2350A), 0 = QFN-80 (RP2350B).
# Matches frank_core2_proto/firmware/master/src/main.c:102.
#
# A case statement rather than an associative array: macOS ships bash 3.2 and
# there is no newer bash on this machine, so `declare -A` is not available.
expected_package_sel() {
    case "$1" in
        master) echo 0 ;;
        slave)  echo 1 ;;
        *)      return 1 ;;
    esac
}

package_name() {
    if [ "$1" = 1 ]; then echo "QFN-60 (RP2350A)"; else echo "QFN-80 (RP2350B)"; fi
}

probe_serial() {
    local role="$1"
    python3 "$PROBE_SH_DIR/devices.py" resolve "$role" \
        | sed -n 's/^[A-Z]*_PROBE_SERIAL=//p'
}

probe_tty() {
    local role="$1"
    python3 "$PROBE_SH_DIR/devices.py" resolve "$role" \
        | sed -n 's/^[A-Z]*_TTY=//p'
}

# oocd <role> <openocd -c args...>
#
# Deliberately does not add "reset halt". Halting stops core 1, and on the
# master that kills the HSTX scanout: the capture card then shows a "no signal"
# colour-bar pattern instead of whatever we were trying to assert on
# (protea/firmware/rp2350/docs/dev.md). Callers that genuinely need a halted
# target ask for it explicitly.

# Which cores rp2350.cfg should create targets for. The RP2350 has both Cortex-M33
# and Hazard3 RISC-V cores behind one DAP, on different AP numbers, and the
# config only attaches to what USE_CORE names. Set OOCD_CORES="rv0 rv1" to talk
# to a target running RISC-V code.
#
# Reflashing after a RISC-V image is loaded still works through the ARM cores:
# the boot ROM starts in the architecture OTP selects (ARM by default) and only
# switches after reading the image header, so `reset halt` catches an ARM core
# in the ROM before the RISC-V image takes over.
: "${OOCD_CORES:=}"

oocd() {
    local role="$1"; shift
    local serial
    serial="$(probe_serial "$role")"
    [ -n "$serial" ] || die "no probe configured for role '$role'"

    local args=(-f interface/cmsis-dap.cfg
                -c "adapter serial $serial"
                -c "adapter speed $ADAPTER_SPEED")
    # USE_CORE and RESCUE have to be set before the target config is sourced;
    # both are read at parse time, not at init.
    [ -n "$OOCD_CORES" ] && args+=(-c "set USE_CORE { $OOCD_CORES }")
    [ -n "${OOCD_RESCUE:-}" ] && args+=(-c "set RESCUE 1")
    args+=(-f target/rp2350.cfg)
    for c in "$@"; do args+=(-c "$c"); done
    "$OPENOCD" "${args[@]}" 2>&1
}

# Is the Cortex-M33 debug interface reachable?
#
# Once a RISC-V image boots, the M33 cores are powered down: the SWD DP still
# answers (DPIDR reads fine) but reads of the ARM AP fail, so openocd reports
# "Failed to read memory at 0xe000ed00 / Examination failed". Everything that
# talks to an ARM target has to notice that rather than misread it as a dead
# board.
arm_reachable() {
    local role="$1"
    ! oocd "$role" "init" "exit" 2>&1 | grep -q "Examination failed"
}

# Put a RISC-V-running chip back under ARM debug control.
#
# rescue_reset sets RESCUE_RESTART in the RP_AP CTRL register, which resets
# everything except the DP and RP_AP and leaves both M33 cores stopped in the
# boot ROM -- before any image header is read, so before the architecture
# switches again. This is the only way back without physical BOOTSEL access,
# and it matters because openocd here has no working RISC-V target support:
# `target create ... riscv -dap` is rejected outright by this build, so once the
# chip is on the Hazard3 cores there is no other route in.
rescue() {
    local role="$1"
    OOCD_RESCUE=1 oocd "$role" "exit" >/dev/null 2>&1 || true
}

# Make sure the ARM side is usable, rescuing first if it is not.
ensure_arm() {
    local role="$1"
    if ! arm_reachable "$role"; then
        echo "==> $role is not answering on the ARM cores; rescue reset"
        rescue "$role"
        arm_reachable "$role" || die "$role: still unreachable after rescue.
       Hold BOOTSEL on that half and power-cycle to recover."
    fi
}

# read_word <role> <address> -> hex value on stdout
read_word() {
    local role="$1" addr="$2"
    oocd "$role" "init" "mdw $addr 1" "exit" \
        | sed -n "s/^${addr}: \([0-9a-f]*\).*/\1/p" \
        | tail -1
}

# assert_half <role>
#
# The two halves are both RP2350 and answer identically over SWD, so openocd
# cannot tell you that a probe was moved from J1 to J3. Only the package select
# distinguishes them, and getting it wrong overwrites the wrong chip's flash --
# silently, and with a plausible-looking success message. Check every time; it
# costs about a second.
assert_half() {
    local role="$1"
    local want
    want="$(expected_package_sel "$role")" \
        || die "unknown role '$role' (expected master or slave)"

    local got
    got="$(read_word "$role" "$SYSINFO_PACKAGE_SEL")"
    [ -n "$got" ] || die "$role: could not read PACKAGE_SEL -- is the board powered?"

    local got_bit=$(( 0x$got & 1 ))
    if [ "$got_bit" != "$want" ]; then
        die "role '$role' expects $(package_name "$want") but this probe is on $(package_name "$got_bit").
       The probes are swapped between J1 and J3, or bench.conf is wrong.
       Refusing to flash: this would overwrite the other half."
    fi
}
