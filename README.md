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
  |    = afboot + DTB + kernel  |      +--------------------------------+
  |      + rootfs, run in place |         HDMI J5   USB-C J8   uSD J7
  |  USB CDC = debug console    |
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

**Done. Linux runs on the RP2350's Cortex-M33, with an HDMI console, a USB
keyboard and a microSD — all served by the second RP2350 over the link — a
PMSAv8 MPU enforcing kernel/user separation and read-only kernel text, and a
root filesystem executed in place out of flash.**

```
~ # cat /proc/cpuinfo
CPU part        : 0xd21          <- Cortex-M33
Hardware        : RP2350 (Device Tree Support)
~ # cat /proc/interrupts
 16:       2808 nvic_irq   0 Edge      rp2350-timer0
```

```
FRANK Linux v1.00
frank login: root
~ # echo FRANK_KEYS_OK; uname -m
FRANK_KEYS_OK
armv7ml
~ # ls /mnt/sd
286  386  APPLE  C64  CMOS.ROM  DOOM  GENESIS  HERETIC
KICKSTART  MSX  QUAKE  SNES  XT  ZX  cpc
```

Read off the HDMI capture card, not off a serial log: the screen above is drawn
by the other RP2350, the keystrokes went the other way down the same link, and
the microSD is on that half too -- this one has no card slot at all.

As far as we know this is the first Linux to run on this core. FDPIC userspace
with a shared libc, interrupt-masked atomics, 8 MB of PSRAM as system RAM, the
microSD the other RP2350 answers for because this half has no card slot, and an
MPU that keeps userspace out of the kernel and the kernel out of its own text:

```
[    0.000000] Using ARM PMSAv8 Compliant MPU. Used 5 of 8 regions
[    1.333656] Kernel text 0x11009000-0x111e7000 read-only,
               data 0x111e7000-0x112cd020 non-executable
~ # mputest 0x1125a000
MPUTEST kernel 0x1125a000 FAULTED sig=11
MPUTEST RESULT protected
```

The root filesystem is romfs in the QSPI flash, mounted straight off an MTD with
no block device in the way — which is what lets the kernel hand a process a
direct mapping of flash and run the code from there. Nothing is copied:

```
~ # cat /proc/self/maps
10819000-1081e000 r-xp  /lib/ld-uClibc-1.0.52.so   <- 0x10.. is flash
1081f000-10876000 r-xp  /lib/libuClibc-1.0.52.so
1096e000-109ce000 r-xp  /bin/busybox
11668000-11670000 rw-p  [stack]                    <- 0x11.. is RAM
```

Every executable page is in the flash window; only writable data occupies RAM.
BusyBox's 384 kB of text costs nothing to run, and 750 kB of libc and shell text
that used to be resident is now not. On a machine with 8 MB that is the
difference between a userspace and a demonstration.

The console is served by core 1: the kernel writes bytes into a ring in SRAM and
core 1 fans them out to USB CDC and, over the link, to the master's terminal.
The USB console stays live alongside HDMI, which is the rule the whole project
runs on — there is always a channel that does not depend on the thing being
debugged.

### Kernel patches

Nine, in `br-external/patches/linux/`. Five make the port exist; four are gaps in
mainline that only show up once an MPU is switched on.

| | |
|---|---|
| `0001` | `ARM_NO_EXCLUSIVES` — LDREX/STREX are unusable outside SRAM here (F6/F7) |
| `0002` | interrupt-masked atomics, including a new `cmpxchg` path: the pre-v6 one uses `swp`, which ARMv7-M does not have |
| `0003` | `ARCH_RP2350` — machine, timer, ring console and link block driver |
| `0004` | `rp2350_defconfig` |
| `0005` | device tree for this board |
| `0006` | **deliver a signal on an MPU fault.** Mainline points the MemManage vector at `__invalid_entry`, which prints a register dump and then spins forever — so with `ARM_MPU` on, the first stray user pointer stops the machine |
| `0007` | **`STRICT_KERNEL_RWX` on PMSAv8.** `ARCH_HAS_STRICT_KERNEL_RWX` is offered only when there is an MMU, so every NOMMU kernel claimed to have no kernel memory protection whether or not its MPU was on |
| `0008` | **an executable region for the flash.** `pmsav8_setup_io()` maps everything that is not RAM as execute-never device memory, so a romfs in flash mounts, reports success, and dies at the first instruction fetch. Mapping it read-only also turned out to be what stops a stray store becoming a QSPI *write* — which is what had been killing the debug port (F26) |
| `0009` | peripherals privileged-only, so a wild userspace pointer cannot reach every register on the chip |

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
| `slave_console.py` | attach across a reset, waiting for the old USB node to go |
| `build-reffw.sh` | build the reference firmware used to validate all of the above |
| `build-afboot.sh` / `build-ioserver.sh` | the two firmwares |
| `flash-slave.sh` | bootloader + kernel + DTB, then boot and assert a shell |
| `check.sh` | the harness gate, against known-good firmware |
| `test-blk.sh` | the microSD, mounted from Linux over the link |
| `test-console.sh` | a shell on HDMI, answering typed input |
| `test-mpu.sh` | MPU on, kernel text read-only, userspace locked out |

Rules the harness enforces, each learned the expensive way:

- **Never `halt` over SWD while asserting on video.** Halting stops core 1, the
  HSTX scanout dies, and the capture shows a "no signal" pattern.
- **Never flash without checking which half you are on.** Both chips are RP2350
  and answer identically over SWD; only `SYSINFO.PACKAGE_SEL` distinguishes
  QFN-80 from QFN-60. `flash.sh` reads it every time and refuses on a mismatch.
- **SWD runs at 1 MHz, not 5.** Once the master drives HDMI — eight differential
  pairs switching at 252 MHz — 5 MHz SWD stops working on that half. It does not
  fail as "SWD is marginal": it fails as a flash write that verifies wrong, and
  by then the erase has happened, so the chip has no valid image, wedges on boot,
  and the next thing to touch it reports something else entirely (F20).
- **Identify the half in one openocd session, 30 ms long.** Two sessions leave
  the image running for most of a second while openocd relaunches, which is long
  enough for the master to start scanning out video; DispHSTX's DMA then fights
  openocd for SRAM and the flash write fails (F19).
- **Assert on something only success can produce.** Two versions of the block
  test passed on failure — `grep frankblk0` matched `No such file or directory`,
  and the next attempt matched the shell echoing the command back.

## Requirements

macOS or Linux host. `openocd`, `ffmpeg`, `cmake`, `arm-none-eabi-gcc`,
`picotool`, Python 3 with `pyserial`, and the Pico SDK. Kernel-side builds run in
a Debian container (Buildroot needs Linux); the harness runs natively.

Written for bash 3.2, which is what macOS ships — no associative arrays.

`make lint` runs what CI runs.

## Layout

```
tools/                      test harness and gates (above)
firmware/afboot-rp2350/     slave bootloader: clocks, QMI/PSRAM, DTB, handover
firmware/master-ioserver/   master: terminal, HDMI, USB HID, microSD, link
firmware/common/            link bus, console ring, core-1 services
br-external/                Buildroot external tree: configs, kernel patches, DTS
hwtests/                    bare-metal experiments behind docs/hw-findings.md
docs/                       hw-findings.md -- measurements, not datasheet quotes
smoke-riscv/                the RISC-V reference boot, kept as a control
```

## How it was built

One assertion, made on progressively more of the real system: *a BusyBox prompt
appears*.

| | gate |
|---|---|
| 0 | the harness itself, against firmware already known to work |
| 1 | RISC-V Linux on the master half — somebody else's known-good code, to prove the board |
| 2 | bare-metal measurements, which is where the atomics plan died |
| 3 | ARM NOMMU + FDPIC under QEMU, no hardware |
| 4 | the same kernel on the slave, over USB CDC |
| 5 | the microSD, served over the link |
| 6 | the shell on HDMI, answering a keyboard |

Phase 2 is the one that mattered. It was meant to confirm that ARM exclusives
work in PSRAM where RISC-V's do not; it showed the opposite, and it showed it
before a single line of the port had been written.

## Known limits

- **Kernel-mode MPU faults are not survivable.** MemManage runs at the same
  priority as SVCall and PendSV so that the return-to-user path is safe (0006);
  the cost is that a fault taken while the kernel is at that priority escalates
  to HardFault and stops. That is the right trade for a kernel bug, but it also
  means `copy_to_kernel_nofault` is not usable here.
- **`STRICT_KERNEL_RWX` is implemented for PMSAv8 only.** On PMSAv7
  `mark_rodata_ro()` says so rather than silently doing nothing.
- **The USB HID path is not machine-tested.** A keyboard enumerates and the
  harness types through the identical path from `terminal_feed_event()` onward,
  but nothing here can press a physical key.
- **Nothing bounds `/tmp`.** The root is read-only romfs, so `/tmp`, `/run` and
  `/var` are ramfs — `tmpfs` needs `SHMEM`, which needs an MMU. ramfs takes no
  `size=`, so a runaway write to `/tmp` consumes the machine's whole 8 MB.
- **There is no `fork()`, and there will not be one.** NOMMU has no address
  translation, so a copied process would hold pointers into its parent. `vfork`
  plus `execve` works and `posix_spawn` is available; anything that needs a real
  copy of the running process — a subshell, a command substitution — has to
  re-exec itself instead. That is why the shell is BusyBox `hush`, why `nano`
  needed patching, and why bash and mc are not here yet.
- **A user process can still fault on its own heap once memory is exhausted.**
  Seen with leaked allocations under 8 MB; the addresses are inside a region the
  MPU grants, so this is not the MPU, and it is not yet attributed.

## Credits

Builds on work by Mikhail Matveev ([rh1.tech](https://rh1.tech)) — the FRANK
boards, the Core 2 bring-up firmware, and the Protea terminal firmware whose
VT/ANSI engine, cell renderer and fonts the master I/O server is built from.
Protea's terminal engine derives from VersaTerm by David Hansel.
`pi-pico2-linux` by Jesse Taube and `cortexm-linux` by Jim Huang are the
reference points for RP2350 Linux and for ARM nommu FDPIC respectively.
