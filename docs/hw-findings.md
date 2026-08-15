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
