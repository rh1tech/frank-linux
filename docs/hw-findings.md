# Hardware findings

Measurements taken on the bench, not quoted from datasheets. Each entry says how
it was obtained so it can be re-run and disputed.

Board: FRANK Core 2 Proto, first assembled unit.
Bench: two CMSIS-DAP probes (master `E6616407E335BB29`, slave `E6635C08CB46BB26`),
MS2109 HDMI capture at native 640x480.

---

## F1. Both halves identified from silicon

`SYSINFO.CHIP_ID` = `0x30004927` on both halves (revision nibble 3).
`SYSINFO.PACKAGE_SEL` = 0 on the master probe, 1 on the slave probe, i.e. QFN-80
(RP2350B) and QFN-60 (RP2350A) respectively. Read over SWD without halting, via
`tools/probe.sh:read_word`.

This is what `tools/flash.sh` checks before every write. Both chips are RP2350
and answer identically over SWD, so a probe moved between J1 and J3 would
otherwise erase the wrong half and report success. The guard is exercised
against a deliberately swapped config, and it refuses.

## F2. Reference firmware reproduces its documented numbers

With the bring-up firmware built at its defaults (252 MHz, PSRAM 133 MHz, flash
66 MHz, USB CDC console) the board reports `LINK OK - error free to 96.1 MiB/s
aggregate`, matching `frank_core2_proto/firmware/README.md` line for line:
48.0 MiB/s each way at 1.00x, 0 byte errors at every divider, control round-trip
141.23 us against a documented 139.7 us.

The harness is therefore calibrated against firmware already known to work,
which is the point of Phase 0: when it later says our own code failed, that
verdict can be trusted.

Also confirmed live in the same run: microSD detected (7422.1 MB), I2S clocking,
HDMI 640x480@60, and a **USB HID keyboard enumerating and mounting** on the
master ("Keyboard detected", protocol=1). Phase 6's input path already works.

## F3. USB HID host costs the master ~3x memory bandwidth and ~20x link latency

The single most consequential measurement so far. Same firmware, same clocks,
same board, differing only in `USB_HID`:

| Measured on the **master** | HID host on | HID host off | Cost |
|---|---|---|---|
| Flash XIP read | 10.2 MiB/s | 32.5 MiB/s | **3.2x** |
| PSRAM write | 4.4 MiB/s | 12.9 MiB/s | 2.9x |
| PSRAM read | 10.2 MiB/s | 30.1 MiB/s | 3.0x |
| Link control round-trip | 2752.98 us | 141.23 us | **19.5x** |

| Measured on the **slave** | HID host on | HID host off | Cost |
|---|---|---|---|
| Flash XIP read | 33.4 MiB/s | 32.7 MiB/s | none |
| PSRAM write | 13.4 MiB/s | 13.1 MiB/s | none |
| PSRAM read | 32.9 MiB/s | 32.1 MiB/s | none |

**The slave is its own control.** `USB_HID` moves *both* halves' consoles from
USB CDC to UART, so if the slowdown were an artifact of blocking UART printf
inside the timed loops, the slave would show it too. It does not move at all.
The only other thing `USB_HID` changes is that the master gains a TinyUSB host
stack, so that is what the master is paying for.

Mechanism not yet confirmed. The leading hypothesis is XIP cache thrash rather
than CPU time: the XIP cache is only 8 KiB, USB interrupts arrive at ~1 kHz, and
a handler executing from flash XIP evicts the lines the measured loop is
streaming through. CPU cost alone does not obviously explain 3x. Phase 2 item 5
should settle it, and the answer matters directly — a Linux IRQ handler running
from flash XIP would thrash the same cache the same way.

### What follows from it

1. **The architecture is right, and now for a measured reason.** Linux on the
   slave gets 32 MiB/s PSRAM that nothing else on the board can disturb. Linux
   on the master would have run at 10 MiB/s once HDMI and a keyboard were
   attached — a 3x penalty on exactly the bandwidth a NOMMU kernel executing
   from PSRAM lives on.
2. **Phase 6 constraint:** the master's link service must tolerate ~2.75 ms
   control round-trips whenever HID is active. Fine for keystrokes at human
   rates; fatal for a protocol that does a control round-trip per byte. The
   console transport must be a streaming ring, not request/response per
   character.
3. **Early evidence for plan risk #2** (IRQ latency under XIP stalls) and for
   its mitigation (force hot IRQ paths into SRAM).

## F6. LDREX/STREX do not work outside SRAM — the ARM side is no better than RISC-V

**The Phase 2 go/no-go test, and it came back negative.** `hwtests/exclusives`,
run on the master at 252 MHz:

```
RESULT excl region=sram           plain_ok=1 ok=1000 failed=0    mean_attempts=1 value_wrong=0
RESULT excl region=psram_cached   plain_ok=1 ok=0    failed=1000 worst=1000      value_wrong=1
RESULT excl region=psram_uncached plain_ok=1 ok=0    failed=1000 worst=1000      value_wrong=1
RESULT clrex sram=1 psram=1
RESULT mpu type=0x00000800 dregion=8 separate=0
RESULT psram bytes=8388608
```

- In SRAM every exclusive succeeds on the **first** attempt, 1000 for 1000.
- In PSRAM **not one succeeds**, in either the cached (`0x11000000`) or the
  uncached (`0x15000000`) window, across 1000 operations of up to 1000 retries.
- `plain_ok=1` in all three regions. Ordinary loads and stores work perfectly in
  PSRAM — verified against `0x00000000`, `0xFFFFFFFF`, `0xA5A5A5A5`,
  `0x5A5A5A5A`, `0xDEADBEEF`. This is the check that makes the result mean what
  it says: "STREX never succeeds here" and "this is not writable memory" look
  identical in the exclusive columns, and only one of them is a CPU property.
- The LDREX/ADD/STREX triple is emitted as a single inline-asm block, so the
  compiler cannot have inserted anything between the pair that would clear the
  monitor and fake this result.
- `value_wrong=1` on PSRAM: after zero successful increments the counter should
  still read 0, and it does not. STREX to memory with no exclusive monitor is
  architecturally UNPREDICTABLE, and this is what that looks like — it may
  perform the store while reporting failure.

So the RP2350's exclusive monitor covers SRAM only, and it covers it for **both**
architectures. The hoped-for asymmetry — that a Cortex-M33's local monitor lives
in the core and would therefore work anywhere — does not hold on this chip.

### Why this is worse than the RISC-V case, and what to do about it

RISC-V AMOs to unsupported memory **fault**, which is precisely why
`pi-pico2-linux` can trap and emulate them. ARM exclusives to unsupported memory
**fail silently**. There is no trap to hook, so the same mitigation is not
available: a kernel with its data in PSRAM would not crash, it would livelock
inside the first `cmpxchg` retry loop with no diagnostic at all.

The fix is better than emulation, and it is well-trodden: **build the kernel's
atomics the pre-ARMv6 way**, with `raw_local_irq_save()` around plain loads and
stores instead of LDREX/STREX. On a uniprocessor that is genuinely atomic —
interrupts off means nothing can preempt the sequence, and `CONFIG_SMP=n` means
there is no second CPU to race with. Linux already contains these paths for
ARMv5; the work is forcing their selection on a v7-M build rather than writing
them. Affected: `atomic_t` ops, `cmpxchg`/`xchg`, and `bitops`. Spinlocks need
nothing — on `CONFIG_SMP=n` they compile away to preempt counting. `futex.h`
already has a non-SMP variant that does not use exclusives.

Two consequences that shape the design:

1. **The `CONFIG_SMP=n`, single-core assumption is now load-bearing**, not a
   simplification. Interrupt-masked atomics are only correct if core 1 never
   touches kernel data.
2. **The core1 link-service rings must stay in SRAM**, which the plan already
   specified for latency reasons. That placement is now also what makes real
   core-to-core atomicity available, since SRAM is the one region where
   exclusives work.

Also confirmed in the same run: the MPU is **PMSAv8, unified, 8 regions**
(`MPU_TYPE=0x00000800`, `SEPARATE=0`), which is the shape `arch/arm/mm/pmsa-v8.c`
expects. And `clrex` correctly causes a subsequent STREX to fail in both SRAM
and PSRAM, so exception-return semantics behave.

Bandwidth from the same run, with no video or USB running: flash XIP read
30.2 MiB/s, PSRAM cached 38.3 MiB/s, PSRAM uncached 36.3 MiB/s. Compare F3: the
same master reads PSRAM at 10.2 MiB/s once HDMI and USB HID are active.

## F7. The replacement for exclusives works, and costs about 10 cycles

Measured in the same run as F6, so the failure and its fix are on one screen:

```
RESULT irqatomic  region=sram          ops=100000 final=100000 correct=1 ns_per_op=39
RESULT irqatomic  region=psram_cached  ops=100000 final=100000 correct=1 ns_per_op=39
RESULT spinatomic region=sram          ops=100000 final=100000 correct=1 ns_per_op=75
RESULT spinatomic region=psram_cached  ops=100000 final=100000 correct=1 ns_per_op=75
```

**Interrupt-masked read-modify-write is correct in PSRAM** — 100000 increments,
final count exactly 100000 — and costs **39 ns/op at 252 MHz, about 10 cycles**.
An LDREX/STREX pair is roughly 4-6 cycles, so the kernel pays about 2x per
atomic and nothing else changes. That is a comfortable price for keeping all
8 MB usable as system RAM.

Notably it is **the same 39 ns in PSRAM as in SRAM**. The counter stays resident
in the XIP cache across the loop, so the mechanism itself carries no XIP
penalty; a cold line would cost a miss, but that cost belongs to the access, not
to the atomicity.

The second row matters for a different reason. **SIO hardware spinlocks work in
every region** — they live in the SIO block rather than in the memory being
guarded — at 75 ns/op. Interrupt masking is only correct on one core; the
spinlocks are correct across both. So the core1 link service can share rings
with the kernel safely even though `CONFIG_SMP=n` makes the kernel's own
atomics single-core-only. Two mechanisms, each right for its job:

| Guarding | Mechanism | Cost |
|---|---|---|
| Kernel data, core 0 only | interrupt masking | 39 ns |
| Rings shared with core 1 | SIO hardware spinlock | 75 ns |
| Anything in SRAM, no sharing | LDREX/STREX | ~4-6 cycles |

## F8. Phase 1 passed: Linux boots on this board — and shows exactly where 8 MB runs out

RISC-V Linux 6.15 boots on the FRANK Core 2 Proto master half to an interactive
BusyBox shell. `tools/flash-smoke.sh` does it unattended: rescue, verify half,
flash, capture, reset, assert.

```
RP2350 Bootloader starting...
PSRAM ID: 5d 53
PSRAM setup complete. PSRAM size 0x800000 (8388608)
Jumping to kernel at 0x11000000 and DT at 0x10001a50
[    0.000000] Linux version 6.15.0 (riscv32-buildroot-linux-uclibc-gcc 14.3.0) #3
[    0.000000]   Normal   [mem 0x0000000011000000-0x00000000117fffff]
[    1.811950] 40070000.serial: ttyAMA0 at MMIO 0x40070000 (irq = 33) is a SBSA
[    4.112153] Run /init as init process
~ #
```

The board hosts a Linux kernel in PSRAM. QMI setup, PSRAM timing, flash layout
and console all work, on our hardware, with the master's PSRAM chip select moved
to GPIO47.

### The shell runs but cannot start a single external command

```
~ # uname -a
nommu: Allocation of length 524288 from process 25 (uname) failed
binfmt_flat: Unable to allocate RAM for process text/data, errno -12
Segmentation fault
```

Every external binary fails the same way. `echo` appeared to work earlier only
because it is a shell builtin — no new process, no allocation.

The numbers say why:

| | |
|---|---|
| RAM present | 8192 kB (2048 pages) |
| Reserved | 3768 kB — kernel image and initramfs sit in the same RAM |
| Managed | 4976 kB |
| Free at the prompt | ~1300 kB |
| Largest contiguous block | **256 kB** |
| Needed per process | **512 kB contiguous** |

With no MMU, `binfmt_flat` must place a process's text and data in one
physically contiguous block. BusyBox's is 512 kB. There is 1.3 MB free and the
largest run is 256 kB, so nothing can start. The shell itself only exists
because it was allocated when memory was still unfragmented.

### This is a measurement of the two ARM-side decisions, not a problem with them

Both mitigations already in the plan attack exactly these two numbers, and this
run turns them from preferences into requirements:

1. **`xipImage` from flash.** This kernel reports `2448K kernel code` living in
   PSRAM. Executing text in place from the 16 MB flash instead returns ~2.4 MB
   of the 8 MB to userspace — roughly doubling what is available.
2. **FDPIC with a shared uClibc-ng, instead of bFLT.** bFLT copies each binary's
   text into RAM per process and demands one contiguous block for text+data
   together. FDPIC separates the segments, so the contiguous requirement drops
   to a process's data alone, and one shared `libc.so` replaces a statically
   linked copy inside every binary. `cortexm-linux`'s whole FDPIC rootfs is
   341 kB against this build's 709 kB `rootfs.cpio`.

So the ARM target is not merely a different instruction set with the same memory
problem — the userspace format is what makes 8 MB workable, and that is the part
this smoke test could not exercise.

## F9. XIP execution, coherence and interrupt dispatch

`hwtests/xip`, master half, 252 MHz. Everything the kernel port needed to know
about running from PSRAM:

| Question | Answer |
|---|---|
| Execute from PSRAM? | **Yes.** Code built at runtime and called returns correctly |
| Instruction coherence after a write? | **Yes**, with `DSB`/`ISB`. `exec()` is safe |
| CPU/DMA coherence in PSRAM? | **Yes**, and both XIP windows agree |
| Unaligned access, SRAM and PSRAM? | **Yes** |
| Vector table relocation? | **Yes.** Moved to SRAM, interrupt dispatched through it |

### Execution rate: the XIP cache is the whole story

| Working set | MIPS | cycles/instruction |
|---|---|---|
| 4 KiB — fits the XIP cache | 246 | **1.02** |
| 96 KiB — cannot fit | 26 | **9.69** |

Code resident in the 16 KiB XIP cache runs at one instruction per cycle. Code
streaming from PSRAM runs at **one tenth** of that. This is the number that
governs how the kernel should be laid out, and it is a far bigger lever than the
clock (F10).

A first attempt at this measured 500 MIPS at 252 MHz — two instructions per
cycle on a single-issue core, which is not a thing. It was a 8 KiB NOP sled:
small enough to sit in the cache, and made of an instruction the core can fold
in the decoder without ever issuing it. The numbers above use `adds` (a real
dependency chain) and a sled far larger than the cache.

### Interrupt dispatch: put handlers in SRAM, for the tail not the mean

Pending-to-entry, in cycles, measured with the DWT cycle counter:

| Handler | mean | **worst** |
|---|---|---|
| flash XIP, idle | 30 | **580** |
| flash XIP, under PSRAM load | 29 | **145** |
| SRAM, idle | 29 | **29** |
| SRAM, under PSRAM load | 29 | **29** |

The means are identical and tell you nothing. The tail is the finding: a
flash-resident handler occasionally takes **20x longer** because dispatch has to
wait on an XIP fetch, while an SRAM-resident one is perfectly flat at 29 cycles
every single time.

So plan risk #2 resolves as: yes, force the kernel's exception entry and hot IRQ
paths into SRAM — not for throughput, which is unaffected, but for jitter. And
it costs nothing, because `__not_in_flash_func` is all it takes.

This measurement replaced an earlier one that used the 1 us timer and reported
"max 1 us idle, 0 us under load" for every case. At 252 MHz one microsecond is
252 cycles, so that resolution could not see a 580-cycle stall at all.

## F10. Overclocking to 504 MHz works, and buys much less than it looks like

The board runs other firmware at 504 MHz, so: does Linux benefit? Measured both
ways on the same binary.

| | 252 MHz | 504 MHz | gain |
|---|---|---|---|
| Execution, cached (4 KiB) | 246 MIPS, cpi 1.02 | 493 MIPS, cpi 1.02 | **2.00x** |
| Execution, streamed from PSRAM | 26 MIPS, cpi 9.69 | 28 MIPS, **cpi 18.00** | **1.08x** |
| PSRAM clock | 126 MHz | 126 MHz | none |
| Flash clock | 63 MHz (div 4) | 63 MHz (div 8) | none |
| IRQ dispatch, SRAM handler | 29 cycles | 29 cycles | 2x in wall time |
| IRQ dispatch, flash handler, worst | 759 cycles | **1427 cycles** | worse |

**Cached execution scales perfectly. PSRAM-resident execution gains 8%.** The
cycles-per-instruction figure says why: it rises from 9.69 to 18.00, so the core
spends exactly as long in absolute time waiting for the same PSRAM. The QMI
divider is derived from the system clock against a 133 MHz ceiling — 252/2 and
504/4 both give 126 MHz — so PSRAM bandwidth is pinned no matter what the core
does.

For this project that means the overclock helps kernel hot paths that fit in
16 KiB of XIP cache, and does essentially nothing for userspace streaming from
PSRAM. Worth having, not worth designing around; the 10x cache/no-cache gap in
F9 is where the real performance lives.

### Three things that must be right, or 504 MHz just dies silently

Each of these cost a debugging cycle here, and each produces the same symptom —
one garbage byte on the console and then nothing, which looks exactly like a
crash:

1. **`vreg_disable_voltage_limit()` before requesting more than 1.30 V.**
   `VREG_VOLTAGE_MAX` on RP2350 *is* `VREG_VOLTAGE_1_30`; a request for 1.65 V is
   silently clamped, and the chip runs underpowered at the target clock.
2. **Program the flash divider for the target clock *before* raising the clock,
   from a RAM-resident function.** `set_sys_clock_khz()` returns into
   flash-resident code, so raising the clock first means the very next
   instruction fetch happens at the new speed with boot2's old divider — at
   504 MHz that asks a W25Q128 for 252 MHz and XIP dies before reaching any code
   that would have fixed it. Setting it first is safe in both directions: until
   the clock rises the flash merely runs slower than necessary.
3. **Pin `clk_peri` to the 48 MHz USB PLL after the clock change.**
   `set_sys_clock_khz()` repoints `clk_peri` at `clk_sys`, so the UART divisor
   moves with the overclock. Protea hit the same trap from the other direction
   (`protea/firmware/rp2350/docs/dev.md`).

Also worth knowing: **400 MHz is not achievable** from a 12 MHz crystal. The VCO
would need 800 MHz, and 800/12 is not an integer feedback divider. 252, 300 and
504 all are.

`afboot-rp2350` will need all of this before it can hand a kernel a clock of its
choosing; the working sequence is in `hwtests/xip/src/main.c`.

## F11. Phase 3 passed: ARM NOMMU + FDPIC boots, and FDPIC does what F8 needed

```
[    0.000000] Linux version 6.15.0 (arm-buildroot-uclinuxfdpiceabi-gcc 14.3.0)
[    0.000000] CPU: ARMv7-M [410fc231] revision 1 (ARMv7M)
[    0.335624] Run /init as init process
Welcome to Buildroot
buildroot login: root
~ # uname -a
Linux buildroot 6.15.0 #10 armv7ml GNU/Linux
```

`tools/qemu-arm.sh`, no hardware. The proof that matters is `/proc/self/maps`:

```
21550000-21555000 r-xp  /lib/ld-uClibc-1.0.52.so
21580000-215d0000 r-xp  /bin/busybox           <- text
21600000-21652000 r-xp  /lib/libuClibc-1.0.52.so
2170c000-2170f000 rw-p  /bin/busybox           <- data, 1.5 MB away from its text
```

Text and data at independently chosen addresses, plus a shared loader and a
shared libc. That is the whole point: where bFLT needed **512 kB contiguous per
process** for text and data together (F8), FDPIC needs only the **12 kB data
segment**, and the text is shared rather than copied.

### Three things that had to be got right, none of them obvious

**The boot wrapper must enable the UART transmitter.** Linux's mps2 earlycon
writes the data register but never enables TX -- it assumes a bootloader already
did. Without that one store the entire boot log, panics included, vanishes into
a disabled UART and the kernel looks like it never started. It had in fact been
booting the whole time. A Cortex-M also has no jump-to-entry reset: it reads the
initial SP and reset handler from address 0, and the kernel links at 0x21008000
with nothing at 0, so a wrapper has to exist at all. `boot/qemu-armv7m/wrapper.S`
is both, and `afboot-rp2350` will do the same job plus clocks and PSRAM.

**`CONFIG_ARM_MPU` had to come off for this target.** QEMU's mps2-an385 is a
Cortex-M3 with PMSAv7, whose region geometry cannot describe this memory map:
`Kernel panic - not syncing: MPU region initialization failure! -12`. Phase 3
proves the toolchain, the binary format and the atomics, none of which need the
MPU. PMSAv8 gets exercised on `mps2-an505` -- a real Cortex-M33 -- and then on
the board.

**Buildroot cannot strip an FDPIC target.** `BR2_STRIP_strip` `depends on
BR2_BINFMT_ELF`, and selecting `BR2_BINFMT_FDPIC` makes the option vanish from
`.config` entirely -- not set to n, absent. On a normal system that is a missed
size optimisation. On NOMMU it is a correctness bug, because ramfs needs one
physically contiguous allocation per file:

```
warn_alloc from __alloc_frozen_pages_noprof
__alloc_pages_noprof from ramfs_nommu_expand_for_mapping
...
unpack_to_rootfs from do_populate_rootfs
```

That is the kernel failing to unpack its own initramfs, because unstripped
`libgcc_s.so.1` is 2.7 MB -- 71% of the rootfs, and an order-10 allocation.
A post-build script doing the strip Buildroot would not:

| | before | after |
|---|---|---|
| target tree | 3976 kB | **1080 kB** |
| `rootfs.cpio` | 3871 kB | **902 kB** |
| `libgcc_s.so.1` | 2765 kB | **99 kB** |

Allocation warnings during boot went from present to none. For a target with
8 MB of RAM this is not tidying, it is the difference between booting and not.

## F12. The Phase 4 console works, proven before Linux exists

Linux cannot drive the RP2350's USB controller -- there is no mainline driver --
so the slave's console is served by **core 1** running TinyUSB CDC, with two byte
rings in SRAM between it and core 0. `hwtests/usbring` stands core 0 in for the
kernel and exercises the whole path on real hardware:

```
RESULT ring base=0x20080000 magic=0x474e5246 version=1
RESULT core1 alive_delta=233317 running=1
```

Core 1 turned over **233,317 service iterations in 200 ms**. The slave's J9 port
enumerates as its own device, addressable by serial like every other instrument
on the bench rather than being a third anonymous "Pico":

```
FRANK Linux console   FRANKLINUX01   /dev/cu.usbmodemFRANKLINUX011
```

And the round trip carries bytes both ways --
`USB -> core1 -> rx ring -> core0 -> tx ring -> core1 -> USB`:

```
sent:     'echo-test-12345\r\n'
received: 'echo-test-12345\r\n'
```

So the Linux side is a ring-buffer tty and nothing more. No USB in the kernel at
all, which was the point.

**No locks and no atomics anywhere in the ring**, deliberately. Each ring has one
producer and one consumer, permanently, on different cores -- and a
single-producer/single-consumer ring needs only ordering, not mutual exclusion.
That is necessary rather than elegant: the kernel runs with interrupt-masked
atomics (F6/F7), which are atomic only against its own core and would protect
nothing against core 1. SIO hardware spinlocks remain the fallback if some later
structure genuinely needs cross-core exclusion.

### Known issue: output written before a terminal attaches is lost

The boot banner did not arrive. Core 1 only drains the TX ring while
`tud_cdc_connected()`, and macOS appears to assert DTR briefly during
enumeration -- long enough to drain the ring into a port nobody is reading.

Harmless for an echo test, not harmless for a kernel console: the boot log is
the primary debugging artifact of Phase 4, and it is written long before anyone
opens the port. The FRANK firmware solves the same problem with
`PICO_STDIO_USB_CONNECTION_WITHOUT_DTR` plus a repeated banner. The ring already
provides the buffering; core 1 needs to stop treating a transient DTR as a
reader. To fix with the Linux driver.

## F13. afboot-rp2350 runs, and four bugs it took to get there

```
=== afboot-rp2350 ===
afboot: sys_clk 252 MHz, flash 63 MHz
afboot: PSRAM 8 MB at 0x11000000
afboot: no dtb at 0x100f0000 (magic 0xffffffff)
afboot: no kernel at 0x10100000 (magic 0xffffffff)
afboot: no kernel to start; halting
```

Clocks, flash divider, PSRAM, the core 1 USB console as the *only* console, and
payload detection correctly reading erased flash. Everything but the kernel
handover. Each of the four things below presented as the same symptom -- silence
after the first line -- and none of them was what it looked like.

### 1. PACKAGE_SEL is invalid while the core is halted in the boot ROM

The `assert_half` guard started refusing the slave and reporting both halves as
QFN-80. It was not the bench:

```
after rescue,      QFN-60 slave: PACKAGE_SEL = 0   (wrong)
plain init, ROM-halted:          PACKAGE_SEL = 0   (wrong)
after reset run + delay:         PACKAGE_SEL = 1   (correct)
```

`SYSINFO.PACKAGE_SEL` is only valid once the boot ROM has run far enough to
latch it, and `rescue_reset` deliberately stops the cores before that. Earlier
in the day the unconditional `rescue` was added to `flash.sh` to stop a bad
image breaking QMI before the debugger could probe -- and it silently broke the
guard.

**It failed open, in the dangerous direction.** With both halves reading 0, a
slave image aimed at the master would have *passed* (expects 0, reads 0) while
the correct target was refused. Fixed by identifying before rescuing, and the
swapped-config test now refuses both roles rather than one.

### 2. DTR is asserted transiently during enumeration

macOS asserts DTR while enumerating, long enough for the bootloader to conclude
a terminal had arrived and print its banner into a port nobody had opened.
Measured directly: `reserved` (core 1's published DTR) reads 0 before the port
is opened and 1 while it is open, but the ring had already emptied.

Buffering does not help, because the whole banner is 236 bytes and TinyUSB's TX
FIFO is 256 -- it is accepted whole and never applies backpressure. Core 1 now
debounces DTR over 250 ms, far longer than the blip and far shorter than a human.

### 3. The obvious "top of SRAM" address is the core stacks

`0x20080000` looks like the natural place for a fixed shared block. It is
`SCRATCH_X`, which the SDK uses for **core 1's stack**. The ring header at the
bottom stayed pristine -- right magic, right version, plausible indices -- while
core 1's own stack grew down through the data it was supposed to be moving.
Moved to `0x2007e000`, the top of *main* SRAM, below both scratch banks.

### 4. psram_init() kills core 1 if core 1 is already running

The real cause of the silence, and the one the FRANK firmware already documents
for a different pair of callers: `psram_init()` takes the QMI out of XIP to
configure chip select 1, and core 1 executing TinyUSB **from flash** has nowhere
to fetch its next instruction from. It simply stops.

```
core1_alive: 001c9f75 -> 001c9f75   (frozen)
tx.head = 0xec, tx.tail = 0x19      (25 bytes out of 236 delivered)
```

25 bytes is exactly the first `ring_puts`. Core 0 wrote everything and reached
its halt loop; core 1 died on the first QMI reconfiguration after it started.

PSRAM must therefore be brought up **before** core 1 is launched, which costs
the ability to report a PSRAM failure on the console -- that is what the LED is
for.

## F14. Phase 4: Linux runs on the RP2350's Cortex-M33

```
[    0.176873] clocksource: Switched to clocksource rp2350-timer0
[    0.204366] frank-ring 2007e000.ring-console: core-1 ring console, core1_alive=1039551
[    0.222696] 2007e000.ring-console: ttyFRK0 at MMIO 0x2007e000 is a FRANK-RING
[    0.223180] printk: legacy console [ttyFRK0] enabled
[    1.170555] Run /init as init process
Welcome to Buildroot
buildroot login: root

~ # cat /proc/cpuinfo
model name      : ARMv7-M rev 0 (v7ml)
CPU implementer : 0x41
CPU part        : 0xd21          <- Cortex-M33
Hardware        : RP2350 (Device Tree Support)

~ # cat /proc/interrupts
           CPU0
 16:       2808 nvic_irq   0 Edge      rp2350-timer0

~ # cat /proc/self/maps
11400000-11452000 r-xp  /lib/libuClibc-1.0.52.so
114c0000-114c8000 rw-p  [stack]
114cc000-114cf000 rw-p  /bin/busybox
```

Memory: 5560K of 8192K available, 965K kernel code, 2420K reserved.

Everything the previous findings predicted, working together: interrupt-masked
atomics in PSRAM (F6/F7), FDPIC userspace with a shared libc (F8/F11), a console
served by core 1 because the USB controller has no Linux driver (F12/F13), and
the clock sequence that 504 MHz taught us (F10).

### The bug that hid behind every other symptom

The kernel booted perfectly and hung in `calibrate_delay_converge`, with no
console output at all. Reading the printk buffer straight out of PSRAM over SWD
-- rather than trying to fix the console first -- showed the kernel was healthy:
machine matched, DTB parsed, memory correct, `rp2350-timer: TIMER0 at 1000000
Hz` registered. It was simply waiting for a tick that never came.

The hardware said the interrupt was ready to fire:

```
NVIC ISER0 = 0x00000001    IRQ 0 enabled
NVIC ISPR0 bit 0 = 1       IRQ 0 pending
PRIMASK    = 0             not masked
BASEPRI    = 0             not masked
ICSR       = 0x0041080b    VECTACTIVE=11 (SVCall), VECTPENDING=16 (our IRQ)
```

Nothing was wrong except two numbers being equal:

```
SHPR2      = 0x80000000    SVCall priority 0x80
NVIC IPR0  = 0x80808080    TIMER0 priority 0x80
```

On ARMv7-M an exception preempts only one of **strictly** higher priority, and
the v7-M kernel runs permanently inside SVCall in Handler mode. An interrupt at
equal priority therefore can never be taken -- it just sits pending forever.

`0x80` is `PICO_DEFAULT_IRQ_PRIORITY`: the pico-sdk's NVIC state was leaking
through the handover into Linux. afboot now disables every interrupt, clears
every pending bit and zeroes every priority before branching, which is what a
bootloader should do anyway. Core 1's NVIC is untouched -- on ARMv8-M it is
core-private, so this cannot disturb the USB service.

### Two smaller ones on the way

**PRIMASK, not BASEPRI.** afboot originally did `cpsid i` before the handover,
which is the obvious way to enter a kernel. Linux on ARMv7-M masks interrupts
with BASEPRI and never touches PRIMASK, so a PRIMASK set by the bootloader is
never cleared by anyone. It now does `cpsie i` instead.

**Reset before attaching, not after.** The console capture opened the port and
then reset the chip, which disconnects USB and invalidates the file descriptor
(`OSError: [Errno 6] Device not configured`). afboot's ten-second wait for DTR
exists precisely so a reader can arrive after the reset.

## F15. The link runs under our own firmware, beside the USB console

```
master: RESULT link master sent=53248 timeouts=12     (slave not yet listening)
slave:  RESULT link slave blocks=64 bytes=65536 bad=0 timeouts=0
```

64 blocks, 65536 bytes, **zero bad bytes and zero timeouts**, verified against an
LFSR pattern each side generates independently from the same seed -- so no
reference data crosses the link being tested.

What is new is not the wire, which F2 already showed at 96.1 MiB/s. It is that
the link now runs under our firmware on the half that also runs Linux, with core
1 serving the USB console at the same time. The two do not interfere.

The master's 12 timeouts in the first run are the sequencing, not the link: the
slave waits for a terminal before starting, so a master reset while the slave is
still waiting finds nobody listening. Attaching to the slave console first and
then resetting the master gives a clean 64 for 64. The doorbell handshake means
neither side needs to agree about absolute time, but it does need the other side
to be running.

One build lesson worth keeping: both ends are built from a single source with
`FRANK_IS_MASTER` selecting the direction, and when that define silently failed
to reach the compiler the master compiled the *slave's* receive path -- then sat
waiting for a doorbell nobody would ring, printing nothing, with the UART never
even configured (`GPIO0 FUNCSEL = 0x1f`, NULL). A missing define looks exactly
like dead hardware.

## F16. Phase 5: a block device on the slave, served by the master over the link

```
~ # ls -l /dev/frankblk0
brw-------    1 root     root      254,   0 /dev/frankblk0
~ # cat /sys/block/frankblk0/size
128
~ # dd if=/dev/frankblk0 of=/dev/null bs=512 count=64
64+0 records out
~ # echo test-write > /tmp/w; dd if=/tmp/w of=/dev/frankblk0 bs=512 seek=10
~ # dd if=/dev/frankblk0 bs=512 count=1 skip=10 | head -1
test-write
```

Reads and writes both complete, and a write read back through the entire path:
Linux -> descriptor in SRAM -> core 1 -> the link -> the master -> the medium
-> back. The medium is a RAM disk on the master for now; the microSD driver
replaces it without the protocol changing.

### Three bugs, and each one taught the same lesson from a different angle

**The receiver must be armed before the sender starts.** `link_bus.h` says so
plainly -- `link_rx_arm()` restarts the state machine and resets the input shift
counter, which is what keeps 32-bit autopush words aligned. The slave armed
*after* sending its request, which is the natural way to write it, and lost
every reply. The master's own log said `doorbell seen` and `served=1 errors=0`:
it received the request and answered correctly into a receiver that did not yet
exist. Fixed by arming first, and by sending the status header and any read data
as a single transfer so there is no second window to miss.

**`add_disk()` needs a major.** Leaving `disk->major = 0` fails with `-EINVAL`
after the queue, tag set and capacity are all correct, so the failure points
nowhere near the cause. `register_blkdev(0, ...)` first.

**Nothing slow may run before the service loop.** `frank_blk_init()` probed the
master for its capacity before core 1 entered its loop, so `tud_task()` was not
being called -- and a link transaction that finds the master absent blocks for
eight seconds. USB could not enumerate, the host gave up, and the console never
appeared: `core1_alive` stuck at 0 with no CDC device at all. The probe now
happens inside the service loop once USB is up, which afboot's wait for a
terminal leaves ample time for.

The common thread is that every one of these presented as silence somewhere
unrelated to the cause -- a lost reply, a device that would not enumerate, a
probe failure with a clean backtrace. Reading state directly out of the hardware
(the ring indices, `core1_alive`, the kernel's own `__log_buf` over SWD) found
each of them; guessing did not.

## F17. The microSD, mounted by Linux over the link

The card is on the master's SPI0 (J7). The half that runs Linux has no slot, so
every sector crosses the link.

```
[    0.296323]  frankblk0: p1
[    0.301135] frank-blk 2007e000.link-block: 15605760 sectors (7802880 KiB) over the link
~ # mount -o ro /dev/frankblk0p1 /mnt/sd
~ # ls /mnt/sd
286  386  APPLE  C64  CMOS.ROM  DOOM  GENESIS  HERETIC
KICKSTART  MSX  QUAKE  SNES  XT  ZX  cpc
```

`frankblk0: p1` is the useful line: the block layer parsed a partition table it
had to read over the link to see, so those are real sectors off a real card and
not something the driver could have invented. The FAT driver then agreed, and
the listing is this board's actual emulator card.

The driver is ChaN's FatFs SPI sample, vendored from the FRANK bring-up
firmware. Only the diskio layer is compiled -- `disk_initialize`, `disk_read`,
`disk_write`, `disk_ioctl` -- because Linux owns the filesystem. There is no FAT
code on the master at all.

Mounted read-only, and the gate keeps it that way: this is a card with real data
on it, and a test that can destroy what it is testing is not a test. Writes are
proven against the master's RAM disk instead, where `dd` to sector 10 and back
returns what was written.

If no card answers, the RAM disk stands in and the console says
`medium ramdisk (NO CARD)` rather than quietly serving 64 kB of pattern that
looks like a working disk.

## F18. Phase 6: a shell on HDMI, answering the keyboard

```
buildroot login: root
~ # echo FRANK_KEYS_OK; uname -m
FRANK_KEYS_OK
armv7ml
~ #
```

Decoded off the capture card at confidence 1.000, with the command typed in
rather than printed. Every layer of the machine is in that loop: the terminal
engine, the link, ttyFRK0, the shell, DispHSTX, and the font decode. `armv7ml`
is the shell on the Cortex-M33 answering for itself.

DispHSTX and Protea's VersaTerm-derived terminal went across unmodified -- the
FRANK master's HDMI pin map is byte-identical to `DISPHSTX_DVI_PINOUT 2`. Only
`term_compat.c` is new, and it is only the question of where the bytes come from
and where keystrokes go. The terminal does not know it is talking to a kernel on
another chip; Linux does not know its console is being drawn by a second
processor.

### The link is a byte pipe in both directions

Console output rides the same doorbell/transaction machinery as the disk, as
op 3. Nothing in the kernel changed for it: core 1 already drained ttyFRK0's ring
to USB, and now a fan-out feeds both consumers, each at its own pace. The USB
console stays live alongside HDMI, which is the same principle every phase has
relied on -- there is always a channel that does not depend on the thing being
debugged.

Keystrokes go back inside the reply of the same transaction. The reply is a
fixed size regardless of how many keys are waiting, because the receiver must
arm for an exact byte count before the sender starts and the slave cannot know
in advance what the master will have to say.

### Four bugs, one shape

**`link_tx_start()` sends `bytes / 4` words and drops the remainder in silence.**
Every earlier caller used sector- and header-sized frames, so nothing noticed for
five phases. The console is the first caller with an arbitrary length: a 37-byte
write put 36 bytes on the wire while the receiver waited for 37, and the leftover
shifted every transfer after it. The screen showed a boot log that started clean
and decayed into fragments of itself. Console frames are padded now and the
function asserts instead of truncating.

**CR and LF configured as 0 does not mean "leave them alone", it means
"discard".** The whole boot log arrived as one line.

**The console fan-out must hold the ring when NOBODY is listening**, which is not
the same as snapping both tails forward. Draining into a buffer no one reads
throws the data away, and the data in question is the boot log -- F12 again,
reintroduced by me and caught by the same blank screen.

**A one-shot capacity probe made correctness depend on boot order.** The master
needs 4.5 s to bring up display, keyboard and card; the slave asked once at 1 s,
blocked five seconds waiting for a reply, and took the console with it. It
retries now, with a 20 ms timeout on the doorbell rather than five seconds:
console and capacity requests run on the core that services USB and must never
stall it, while block requests can still wait seconds for a busy card.

Every one of these presented as silence somewhere unrelated to its cause.

## F23. A usable userspace, stage 0: the terminal holds up

```
VI_LINE_ALPHA
VI_LINE_BETA
~
... 22 tilde lines ...
- /tmp/t 1/2 50%
```

Decoded off the capture card at confidence 1.000. Two content lines, twenty-two
tildes and a status row is exactly 25, which is the check that matters: the
console is a ring in SRAM with no driver that knows anything about geometry, so
`TIOCGWINSZ` returns 0x0 and every full-screen program falls back to 80x24 --
one row short, status line in the wrong place. `stty rows 25 cols 80` from
`/etc/profile.d` fixes it once for vi, nano and mc alike.

The VersaTerm engine needed nothing. Audited against what vt102 terminfo asks
for, it already handles cursor addressing, erase, insert/delete line and
character, scroll regions, SGR, save/restore, and `ESC[6n` -- the cursor
position report, which is how a program discovers the screen size when the tty
will not tell it. `v/vt102` is in Buildroot's default terminfo set, so `TERM`
needed nothing either.

### The console link was sized for scrolling text

`LINK_CON_MAX_TX` was 64 bytes, which is ample for a shell emitting a line at a
time and hopeless for a program that repaints the screen. A full 80x25 repaint
with attributes is several kB, and every transaction costs a round trip: 141 us
idle, 2753 us with the master's USB HID host running (F3). At 64 bytes that is
64 round trips, a sixth of a second per repaint with a keyboard plugged in, and
an editor that feels broken. Now 512, so eight.

### What it cost

| | before | after |
|---|---|---|
| MemFree | 2708 kB | 1972 kB |
| MemAvailable | 2388 kB | 1692 kB |
| rootfs.cpio | 912 kB | 1314 kB |
| applets | 162 | 176 |

The stock NOMMU BusyBox config ships 162 applets and no editor at all -- no vi,
no less, no awk, no find, no tar. That is not a size decision, it is just what
the default happens to contain.

1692 kB of headroom is enough for nano and nowhere near enough for mc plus
glib2, which is what puts /usr/local on the card in the next stage.

### ncurses does not think this machine can link a shared library

```
checking which arm-buildroot-uclinuxfdpiceabi-gcc option to use... -fPIC
configure: error: Shared libraries are not supported in this version
```

Both lines are from the same configure run. It probes the compiler, gets -fPIC,
and then decides from a case on the host triple -- where `uclinuxfdpiceabi`
matches neither `linux*` nor `gnu*` and falls into `(*)`, which sets
`CC_SHARED_OPTS=unknown`. Other cases in the same file already list `uclinux*`
beside the linux variants, so it is an omission rather than a policy.

Shared libraries are not optional here: one `libc.so` shared across processes is
what lets an 8 MB NOMMU system run more than one program (F8, F11).

## F22. CONFIG_ARM_MPU: it works, and mainline cannot survive it

```
[    0.000000] Using ARM PMSAv8 Compliant MPU. Used 5 of 8 regions
~ # mputest 0x111698d0
MPUTEST own    0x11517dfc read 0xa5a5a5a5 OK
MPUTEST kernel 0x111698d0 FAULTED sig=11
MPUTEST RESULT protected
~ #
```

On NOMMU without an MPU there is nothing at all between a user process and
kernel memory. With PMSAv8 there is, and the shell prompt after the fault is the
part that matters: the process died, the machine did not.

### What the kernel actually programs

`pmsav8_setup()` does not only map system RAM, which is what the defconfig
comment here used to assume. It maps the whole 4 GB as execute-never device
regions and subtracts RAM and the kernel from that, so nothing board-specific is
needed. Read back over SWD while running:

| rgn | range | AP | XN | what |
|---|---|---|---|---|
| 1 | `0x11008180-0x112cb000` | PL1 RW, **PL0 none** | no | the kernel |
| 2 | `0x00000000-0x10ffffff` | PL0 RW | yes | flash XIP and low IO |
| 3 | `0x11800000-0xffffffff` | PL0 RW | yes | SRAM (the console ring at `0x2007e000`) and peripherals |
| 4 | `0x11000000-0x11008180` | PL0 RW | no | the DTB |
| 5 | `0x112cb020-0x117fffe0` | PL0 RW | no | userspace |

`MPU_TYPE = 0x00000800` (8 regions, unified) and `MPU_CTRL = 0x1`: enabled, and
**PRIVDEFENA clear**, so there is no background region even for the kernel --
every privileged access has to land in one of those five. The console ring and
the block descriptor are covered by region 3 and keep working; both gates still
pass with the MPU on.

### The vector went nowhere

`arch/arm/kernel/entry-v7m.S` pointed vector 4 (MemManage) at `__invalid_entry`,
which prints a register dump and then executes `1: b 1b`. So the protection
worked exactly once:

```
Unhandled exception: IPSR = 00000004 LR = fffffffd
CPU: 0 UID: 0 PID: 34 Comm: mputest Not tainted 6.15.0
PC is at 0x11488a44
```

and nothing further, ever. `IPSR = 4` is MemManage, `LR = fffffffd` says the
fault came from user thread mode, and `r3`/`r6` hold the address that was read.
The offending process was stopped, and so was everything else. Protection you
cannot survive is not much better than none, and it makes the option unusable in
practice: any userspace bug takes the system down.

Patch 0006 routes the vector at a handler that reads MMFSR/MMFAR, acknowledges
the sticky status bits and calls `arm_notify_die()` -- SIGSEGV for user mode,
`die()` for kernel mode. Returning straight to the faulting instruction is not
an option, because it would fault again forever; the exit is through
`ret_to_user_from_irq`, which delivers the signal first.

### Priority, again

MemManage resets to priority 0, above everything. SVCall and PendSV are lowered
to 0x80 by `proc-v7m.S`, and the return-to-user machinery is written for code
running there -- PendSV is where it normally runs. A handler that can deliver a
fatal signal and schedule must not do that from a higher-priority exception, or
the switch happens with the exception still active. So MemManage is lowered to
0x80 as well.

The cost is that a MemManage taken while the kernel itself is at 0x80 cannot
preempt and escalates to HardFault. That is the correct trade -- a kernel-mode
MPU violation is a kernel bug and should stop the machine loudly, while a
user-mode one has to be survivable -- and it is the same equal-priority rule
that bit the bootloader from the other direction (F13): equal priority does not
preempt.

### An aside worth not misreading

The boot log still says "This architecture does not have kernel memory
protection." That is about `STRICT_KERNEL_RWX` -- read-only kernel text within
kernel mode -- and is unrelated to what the MPU is doing here, which is keeping
*user* mode out.

## F21. The capacity probe has to finish before Linux asks

Deferring the link until handover (F18) introduced a race with the kernel. The
block driver reads the capacity exactly **once**, about 0.26 s after handover,
and declines to register permanently if it is still zero. Core 1 retries the
probe every 250 ms, so the two raced and the machine came up with no disk:

```
frank-blk 2007e000.link-block: no storage offered by the master; not registering
```

which reads like a missing card rather than a timing bug. afboot now waits (up
to a second) for a non-zero capacity before entering the kernel, and says what
it got. The wait only costs anything when the master is genuinely absent, in
which case there is no disk to miss.

Worth noting as a pattern: both this and the one-shot probe in F18 are the same
mistake in different places -- a value read once, at a moment nobody chose, from
a peer that comes up on its own schedule.

## F20. The real cause: SWD at 5 MHz stops working once HDMI runs

Everything in F19 -- the failed flash writes, the wedged master, the
"Could not load data into target bounce buffer", the chip that could not even be
identified -- came from one thing, and it was not the firmware.

The harness ran SWD at 5 MHz. Once the master drives HDMI, eight differential
pairs switching at 252 MHz on GPIO12..19, that stops being reliable on the
master half. Measured directly, with the display running:

| adapter speed | result |
|---|---|
| 5000 kHz | `Error connecting DP: cannot read IDR`, every time |
| 1000 kHz | examination succeeds, every time |
| 200 kHz | examination succeeds, every time |

The default is 1000 now. Three consecutive clean runs of the Phase 6 gate
followed, after an afternoon in which no two failures looked alike.

The reason it was so expensive to find is that a marginal SWD link does not fail
as "SWD is marginal". It fails as a flash write that verifies wrong, and by then
the erase has happened, so the half has no valid image, wedges on boot, and the
next thing to touch it reports something else entirely. Every symptom pointed at
whatever code had most recently changed.

The lesson worth keeping: when unrelated failures start appearing together after
a change in what the hardware is *doing* rather than what it is *running*,
suspect the measurement path before the thing being measured.

## F19. What running a display does to the flashing harness

The master half became much harder to flash once it drove HDMI, and the reason
was in the harness rather than the firmware.

`assert_half` identifies which half a probe is on before flashing, and to do
that it has to let the ROM run long enough to latch `PACKAGE_SEL`. It did so by
resetting, exiting openocd, and reading the register in a second session -- so
the image on the flash ran for however long openocd took to relaunch, most of a
second. That is ample for the master to start scanning out video, and DispHSTX's
DMA keeps running afterwards: halting a core does not halt DMA. openocd then
cannot place its flash algorithm in SRAM, which surfaces as

```
Error: Failed to write memory at 0x2001d6fc
Error: Could not load data into target bounce buffer
```

The erase has already happened by then, so the chip is left with no valid image.
It boots nothing, wedges, and every later step reports some other problem
entirely -- which is how this cost an afternoon: the visible failures were
always somewhere else.

Reset, wait and halt now happen in one session with a 30 ms window, which is far
more than the ROM needs and far less than any image needs to bring up a display.
Writes are retried, which is safe precisely because nothing is accepted until
`verify_image` agrees.

Two smaller ones with the same moral. A wedged chip cannot be identified at all
-- openocd reports `DP initialisation failed` and every read comes back empty --
so identification has to rescue first and then still reset, because reading
`PACKAGE_SEL` while the chip is held in rescue returns 0 for either package and
passes for both halves. And an empty read used to kill the calling script
through `set -e` with nothing printed, so a wedged board looked exactly like a
script that stopped for no reason after `==> flashing slave`.

**Do not poke registers speculatively to fix an intermittent problem.** Writing
`DMA_CHAN_ABORT` to stop the scanout looked like the obvious fix and made things
strictly worse: aborting a channel parked on a DREQ that never comes stalls the
bus, and the next several failures were caused by the fix rather than the fault.

## F4. Master flash contents at project start

Both halves arrived carrying an unrelated project's firmware (slave: a SID/6581
emulator printing to USB CDC; master: something linking FatFs). Overwritten with
the owner's explicit go-ahead and no backup taken, at their instruction.

## F5. The slave's UART console is unwired, not broken

The master's UART0 (J2) reaches its probe's CDC bridge and works. The slave's
UART1 (J4) produces nothing, although the slave is demonstrably healthy — it
answers the link, runs its self-test, ships results back, and its firmware calls
`printf` from boot onward.

Read over SWD, the slave's UART1 is configured **identically** to the master's
working UART0:

| Register | Slave UART1 | Master UART0 (works) |
|---|---|---|
| GPIO CTRL (TX pin) | `0x02` = FUNCSEL UART | `0x02` |
| `UARTIBRD` / `UARTFBRD` | `26` / `3` = 115200 @ 48 MHz | `26` / `3` |
| `UARTLCR_H` | `0x70` = 8N1, FIFOs on | `0x70` |
| `UARTCR` | `0x301` = UARTEN, TXE, RXE | `0x301` |

So the slave is transmitting 115200 8N1 on GPIO24 and nothing is listening. This
is wiring between header J4 and the second probe's UART pins, and nothing else.

**Needed before Phase 4**, where the slave's J4 console is the primary gate
channel for ARM kernel bring-up and the independent debug path every later phase
relies on:

```
slave J4 pin 1 (GPIO24, TX)  ->  probe 2 UART RX
slave J4 pin 3 (GPIO25, RX)  <-  probe 2 UART TX     (for typing into the shell)
slave J4 pin 2 (GND)         <-> probe 2 GND
```

Probe 2 is the Pico running debugprobe, USB serial `E6635C08CB46BB26`.

## Reproducing

```sh
tools/build-reffw.sh --clean            # USB_HID=1, consoles on UART
USB_HID=0 tools/build-reffw.sh --clean  # control, consoles on USB CDC
tools/flash.sh slave  build/reffw-hid1/slave/frank-core2-slave.elf
tools/flash.sh master build/reffw-hid1/master/frank-core2-master.elf
tools/reset.sh both
python3 tools/console.py capture master --seconds 40
tools/screen.py --png logs/screen.png
```

Two upstream bugs in the reference firmware are worked around by
`tools/build-reffw.sh` rather than patched in place, because that tree is not
under git and edits there would not be revertible:

1. Five sources include `frank_core2_board.h`; the file was renamed to
   `frank_core2_proto_board.h` and no include site was updated. The firmware does
   not compile as it stands. Worked around by `firmware/compat/`.
2. Neither CMakeLists puts `USB_HID_ENABLED` in `target_compile_definitions`,
   but both `main.c` files guard on the macro. `USB_HID=1` therefore switches the
   CDC stdio library off in CMake while the C still compiles as a CDC build: the
   slave fails on the missing `pico/stdio_usb.h`, and the master compiles but
   never initialises the HID host and writes its console to a USB CDC that is not
   enabled. Worked around by defining the macro in `CFLAGS`.
