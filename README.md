# frank-linux

NOMMU Linux on the [FRANK Core 2 Proto](https://github.com/rh1tech/frank) board:
two RP2350s, 8 MB PSRAM each, joined by a 96 MiB/s parallel link. HDMI console,
USB keyboard, BusyBox shell.

Nobody has run Linux on the RP2350's Cortex-M33 cores. `pi-pico2-linux` does it
on the Hazard3 RISC-V side. This takes the ARM path — ARMv7-M/ARMv8-M NOMMU,
PMSAv8 MPU, FDPIC userspace with shared uClibc-ng.

The case for ARM was partly that its exclusive monitor lives inside the core
rather than on the bus, so atomics would work in PSRAM where RISC-V's do not.
**That turned out to be false on this chip** — measured, F6 below. What survives
is the rest, and it is enough: mainline already has `ARM_NVIC` and
`pmsa-v8.c` where the RISC-V port had to invent an interrupt controller, the
RP2350 boots ARM by default, and — the part that actually decides it — FDPIC with
a shared libc is what makes 8 MB of RAM hold a usable userspace at all (F8).

## Architecture

```
        RP2350A "slave" (U6)                 RP2350B "master" (U3)
  +-----------------------------+      +--------------------------------+
  | Linux, NOMMU, PMSAv8        |      | pico-sdk C, no Linux           |
  |  core0: kernel + FDPIC user |      |  core0: link, USB HID host,    |
  |  core1: link service (SRAM) |<====>|         microSD, I2S           |
  |  8 MB PSRAM @ 0x11000000    | 96   |  core1: HSTX DVI scanout       |
  |    = system RAM             | MiB/s|  Protea terminal engine        |
  |  16 MB flash @ 0x10000000   |      |  8 MB PSRAM: free              |
  |    = xipImage + DTB         |      +--------------------------------+
  |  UART1 J4 = debug console   |         HDMI J5   USB-C J8   uSD J7
  +-----------------------------+
```

Linux never touches HDMI, USB or SD. It writes bytes to a serial-like device and
the master renders them: the master is a **terminal**, not a framebuffer. The
RP2350 USB host controller has no mainline Linux driver, and the master already
has a working VT/ANSI terminal, so this is both the cheaper and the better split.

It is also the measured one. With HDMI running and a keyboard attached, the
master's PSRAM read collapses from 30 to 10 MiB/s while the slave stays at 32 —
a 3x difference on exactly the bandwidth a kernel executing from PSRAM lives on.
See [docs/hw-findings.md](docs/hw-findings.md) F3.

## Status

**Phase 5 complete. Linux runs on the RP2350's Cortex-M33, with storage served
over the inter-chip link.**

```
~ # cat /proc/cpuinfo
CPU part        : 0xd21          <- Cortex-M33
Hardware        : RP2350 (Device Tree Support)
~ # cat /proc/interrupts
 16:       2808 nvic_irq   0 Edge      rp2350-timer0
```

```
~ # ls -l /dev/frankblk0
brw-------  1 root root  254, 0  /dev/frankblk0
~ # dd if=/tmp/w of=/dev/frankblk0 bs=512 seek=10 && \
    dd if=/dev/frankblk0 bs=512 count=1 skip=10 | head -1
test-write
```

As far as we know this is the first Linux to run on this core. Console served by
core 1 over USB CDC, FDPIC userspace with a shared libc, interrupt-masked
atomics, 8 MB of PSRAM as system RAM, and a block device the other RP2350
answers for -- because the slave half has no storage of its own.

`tools/check.sh` — the harness, validated against known-good firmware:

```
== 1. bench instruments ==   ok   probes and capture card resolve by serial
== 2. Protea fonts ==        ok   vga 8x16, ega 8x14, cga 8x8
== 3. screen decoder ==      ok   synthetic frames round-trip exactly
== 5. flash both halves ==   ok   verified OK
== 6. master console ==      PASS matched /LINK OK/
== 7. HDMI capture ==        ok   307200 bytes, luma 0..255
PASS  harness validated against known-good firmware
```

`tools/flash-smoke.sh` — RISC-V Linux 6.15 to an interactive BusyBox shell,
unattended, on the master half with its PSRAM at GPIO47:

```
PASS  Phase 1: RISC-V Linux booted to a shell on the master half
```

`tools/qemu-arm.sh` — ARM NOMMU Linux 6.15, FDPIC userspace, no hardware:

```
Linux buildroot 6.15.0 #10 armv7ml GNU/Linux
PASS  Phase 3: ARM NOMMU kernel booted to a shell under mps2-an385
```

Two hardware results shape everything after it, both in
[docs/hw-findings.md](docs/hw-findings.md):

- **F6/F7 — LDREX/STREX do not work outside SRAM**, on the Cortex-M33 exactly as
  on the Hazard3 cores. Ordinary loads and stores in PSRAM are fine; not one
  exclusive succeeds in 10^6 attempts. ARM exclusives fail *silently* where
  RISC-V AMOs fault, so there is nothing to trap and emulate. The kernel will use
  interrupt-masked atomics instead — measured correct in PSRAM at 39 ns/op, about
  10 cycles. See [docs/atomics-port.md](docs/atomics-port.md).
- **F11 — FDPIC does what F8 needed.** `/proc/self/maps` shows BusyBox's text and
  data at independently placed addresses with a shared libc, so per-process
  contiguous demand is a 12 kB data segment rather than a 512 kB whole binary.
- **F8 — 8 MB runs out exactly where predicted.** The smoke-test shell cannot
  start a single external command: `binfmt_flat` needs 512 kB contiguous per
  process and the largest free run is 256 kB, because the kernel image and
  initramfs occupy 3.7 MB of the same RAM. This is the measurement that turns
  `xipImage`-from-flash and FDPIC-with-shared-libc from preferences into
  requirements.

## The bench

Every gate is machine-checked. Two CMSIS-DAP probes flash, reset and trace the
halves independently; an MS2109 card captures HDMI at native 640x480, so frames
arrive pixel-exact and decode straight back into text.

Instruments are addressed by **USB serial number**, never by `/dev` node or
enumeration order — tty names derive from the USB location ID and move on
replug, and with two identical-protocol probes attached "the first one" is not
an identity. `tools/bench.conf` maps role to serial; everything else discovers.

| Tool | Does |
|---|---|
| `devices.py` | role -> USB serial -> tty, with a guard against a registry walk that stops short |
| `probe.sh` | shared SWD helpers; `assert_half` refuses to flash the wrong chip |
| `flash.sh` / `reset.sh` | program or reset one half by role |
| `console.py` | timestamped capture, injection, and `wait` assertions |
| `fontgen.py` | load Protea's font sheets, geometry detected and shape-checked |
| `screen.py` | HDMI frame -> 80x25 text, with a confidence floor |
| `test_screen.py` | decoder round-trip, no hardware needed |
| `build-reffw.sh` | build the reference firmware used to validate all of the above |
| `check.sh` | the whole gate |

Two rules the harness enforces, both learned from the existing code:

- **Never `halt` over SWD while asserting on video.** Halting stops core 1, the
  HSTX scanout dies, and the capture shows a "no signal" pattern.
- **Never flash without checking which half you are on.** Both chips are RP2350
  and answer identically over SWD; only `SYSINFO.PACKAGE_SEL` distinguishes
  QFN-80 from QFN-60. `flash.sh` reads it every time and refuses on a mismatch.

## Requirements

macOS or Linux host. `openocd`, `ffmpeg`, `cmake`, `arm-none-eabi-gcc`,
`picotool`, Python 3 with `pyserial`, and the Pico SDK. Kernel-side builds run in
a Debian container (Buildroot needs Linux); the harness runs natively.

Written for bash 3.2, which is what macOS ships — no associative arrays.

## Layout

```
tools/            test harness (above)
firmware/compat/  shim for a stale include in the reference firmware
docs/             hw-findings.md -- measurements, not datasheet quotes
board/ configs/ package/    Buildroot external tree (Phase 3 onward)
hwtests/          Phase 2 bare-metal experiments
```

## Plan

Phases and gates: see the plan file. Each gate is the same assertion made on
progressively more of the real system — *a BusyBox prompt appears* — on the
slave's UART under RISC-V, in QEMU under ARM, on the slave's UART under ARM,
over the link, and finally decoded off the HDMI capture with a USB keyboard
driving it.

## Credits

Builds on work by Mikhail Matveev ([rh1.tech](https://rh1.tech)) — the FRANK
boards, the Core 2 bring-up firmware, and the Protea terminal firmware whose
VT/ANSI engine, cell renderer and fonts the master I/O server is built from.
Protea's terminal engine derives from VersaTerm by David Hansel.
`pi-pico2-linux` by Jesse Taube and `cortexm-linux` by Jim Huang are the
reference points for RP2350 Linux and for ARM nommu FDPIC respectively.
