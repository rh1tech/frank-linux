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
# SWD clock, in kHz.
#
# 1 MHz, not the 5 MHz this used to run at. Once the master drives HDMI -- eight
# differential pairs switching at 252 MHz on GPIO12..19 -- SWD at 5 MHz becomes
# unreliable on that half: reads fail intermittently, and openocd reports it as
# "Error connecting DP: cannot read IDR", "Failed to read memory at 0x...", or
# "Could not load data into target bounce buffer" part way through a flash
# write. The last one is the damaging case, because the erase has already
# happened: the chip is left with no valid image, boots nothing, wedges, and
# every step after that reports some unrelated problem.
#
# Measured directly: with the display running, examination fails every time at
# 5000 and succeeds every time at 1000 and 200. The cost is a slower flash
# write, which is worth it.
: "${ADAPTER_SPEED:=1000}"

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

# Resolving a probe reads the live USB tree, and that tree moves: the slave's
# CDC console disappears and comes back on every reset, and devices.py refuses
# to answer while it cannot account for every node it sees. Asking again a
# second later is almost always enough, and failing on the first glance turns a
# passing test into "no probe configured for role 'slave'".
probe_serial() {
    local role="$1"
    local out i
    for i in 1 2 3; do
        out="$(python3 "$PROBE_SH_DIR/devices.py" resolve "$role" 2>/dev/null \
               | sed -n 's/^[A-Z]*_PROBE_SERIAL=//p')"
        [ -n "$out" ] && { echo "$out"; return 0; }
        sleep 1
    done
    return 1
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
    # Two different failures mean the same thing. A wedged core reports
    # "Examination failed"; a chip wedged badly enough that the debug port
    # itself stalls reports "DP initialisation failed" instead, and only the
    # ROM rescue clears either.
    ! oocd "$role" "init" "exit" 2>&1 \
      | grep -qE "Examination failed|DP initialisation failed"
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
# silently, and with a plausible-looking success message.
#
# PACKAGE_SEL is only valid once the boot ROM has run far enough to latch it.
# A chip stopped in the ROM -- which is exactly what rescue_reset leaves behind
# -- reads 0 whatever package it is. Measured on this bench:
#
#   after rescue,  QFN-60 slave: PACKAGE_SEL = 0   (wrong)
#   after reset run + delay:     PACKAGE_SEL = 1   (correct)
#
# That is worse than merely unreliable, it fails *open*: with both halves
# reading 0, a slave image aimed at the master would pass the check (expects 0,
# reads 0) while the correct target was refused. So let the chip start, read,
# and only then rescue it back into the ROM for the flash itself.
assert_half() {
    local role="$1"
    local want
    want="$(expected_package_sel "$role")" \
        || die "unknown role '$role' (expected master or slave)"

    # Let the ROM run just long enough to latch the package, then stop.
    #
    # Reset, wait, read and halt all happen in ONE openocd session, because the
    # window is what matters. Two sessions leave the image running for however
    # long it takes to launch openocd again -- most of a second -- and that is
    # ample for the master to start scanning out video. DispHSTX's DMA then
    # keeps running through the rescue and fights openocd for SRAM, which shows
    # up as "Could not load data into target bounce buffer" and a half-erased
    # flash: the chip then boots nothing, wedges, and every later step reports
    # some other problem instead.
    #
    # 30 ms is far more than the ROM needs to latch PACKAGE_SEL and far less
    # than any image needs to bring up a display.
    # Identify, rescuing as needed, and never fail silently.
    #
    # Two things made this the worst step to get wrong. A wedged chip cannot be
    # identified at all -- openocd reports "DP initialisation failed" and every
    # read comes back empty -- so it has to be rescued first. And an empty read
    # used to kill the calling script through `set -e` with nothing printed at
    # all, so a wedged board looked like a script that simply stopped after
    # "==> flashing slave".
    #
    # Rescuing does NOT weaken the check. The reset afterwards still lets the
    # ROM latch PACKAGE_SEL from the package pin, which is the thing being read.
    # What must never happen is reading it while the chip is held in the rescue
    # state, where it reads 0 for either package and the check passes for both
    # halves -- failing open, on the one test whose whole job is to stop us
    # writing a slave image onto the master.
    local got=""
    local attempt
    for attempt in 1 2 3; do
        arm_reachable "$role" || rescue "$role" >/dev/null 2>&1 || true

        # Reset, wait and halt in ONE session: the window is what matters. Two
        # sessions leave the image running for however long openocd takes to
        # relaunch -- most of a second -- and that is ample for the master to
        # start scanning out video. DispHSTX's DMA then keeps running through
        # the rescue and fights openocd for SRAM, which surfaces as "Could not
        # load data into target bounce buffer" and a half-erased flash: the chip
        # boots nothing, wedges, and every later step reports some other
        # problem instead. 30 ms is far more than the ROM needs to latch
        # PACKAGE_SEL and far less than any image needs to bring up a display.
        oocd "$role" "init" "reset run" "sleep 30" "halt" "exit" >/dev/null 2>&1 || true

        # The target is left halted, so the gap before the next session costs
        # nothing: the image is stopped 30 ms in and stays stopped.
        got="$(read_word "$role" "$SYSINFO_PACKAGE_SEL" 2>/dev/null || true)"
        [ -n "$got" ] && break
        echo "    $role did not answer (attempt $attempt); rescuing" >&2
        rescue "$role" >/dev/null 2>&1 || true
    done

    if [ -z "$got" ]; then
        # No ARM answer after a reset means a RISC-V image took the cores. The
        # package cannot be confirmed in that state; say so rather than pretend.
        die "$role: cannot confirm which half this is -- the ARM cores did not
       answer after reset, which means a RISC-V image is running. Re-run with
       FRANK_ALLOW_UNVERIFIED=1 if you are certain of the wiring."
    fi

    local got_bit=$(( 0x$got & 1 ))
    if [ "$got_bit" != "$want" ]; then
        [ "${FRANK_ALLOW_UNVERIFIED:-}" = 1 ] && {
            echo "WARN  $role: package mismatch overridden by FRANK_ALLOW_UNVERIFIED" >&2
            return 0
        }
        die "role '$role' expects $(package_name "$want") but this probe is on $(package_name "$got_bit").
       The probes are swapped between J1 and J3, or bench.conf is wrong.
       Refusing to flash: this would overwrite the other half."
    fi
}
