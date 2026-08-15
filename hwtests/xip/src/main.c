/*
 * Phase 2, items 2 / 5 / 6 / 7: what the kernel port needs to know about XIP.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Four questions, each of which changes what we build:
 *
 *   exec    Can code execute from PSRAM at all, and how fast? Userspace lives
 *           there, so "no" would end the design.
 *
 *   icoh    Is the XIP cache coherent for instruction fetch after a write?
 *           Loading a program is writing bytes and then jumping to them. If the
 *           I-side can see a stale line, every exec() is a lottery.
 *
 *   dcoh    Is it coherent between the CPU and DMA? The link service on core 1
 *           moves data by DMA into memory the kernel reads.
 *
 *   irq     What is interrupt latency when the core is stalled on an XIP miss?
 *           This decides how much of the kernel has to be forced into SRAM --
 *           and it matters more now than when the plan was written, because
 *           interrupt-masked atomics (see docs/atomics-port.md) add an
 *           interrupts-off window to every atomic operation.
 *
 *   vtor    Can the vector table be relocated? The bootloader must do this to
 *           hand over to a kernel whose vectors are not at the reset address.
 */

#include <stdio.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/scb.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#include "hardware/vreg.h"
#include "pico/stdlib.h"

#include "psram_init.h"

#ifndef CPU_SPEED
#define CPU_SPEED 252
#endif

#define XIP_FLASH_BASE          0x10000000u
#define XIP_PSRAM_BASE          0x11000000u
#define XIP_PSRAM_NOCACHE_BASE  0x15000000u

/* Scratch regions in PSRAM. The gap is not decoration: the streamed exec test
 * builds a 96 KiB instruction sled at CODE_OFFSET, and with DATA_OFFSET only
 * 64 KiB above it the sled ran straight through the coherence test's buffer.
 * The result looked like a cache-coherence anomaly and was self-inflicted. */
#define CODE_OFFSET   0x00010000u
#define DATA_OFFSET   0x00100000u

typedef int (*fn_int_int)(int);

/*
 * Build `adds r0, #1; bx lr` at `dst` and return a callable pointer.
 *
 * Hand-assembled rather than memcpy'd from a compiled function because a
 * compiled one may be built with literal pools or PC-relative references that
 * break when moved. Two instructions with no operands beyond r0 relocate
 * trivially, which keeps this a test of the memory system and not of my
 * relocation.
 *
 * The +1 on the pointer is the Thumb bit: ARMv8-M has no ARM mode, and a branch
 * to an even address raises a UsageFault instead of executing.
 */
static fn_int_int build_increment(volatile uint16_t *dst)
{
    dst[0] = 0x3001;    /* adds r0, #1 */
    dst[1] = 0x4770;    /* bx lr       */
    __dmb();
    __isb();
    return (fn_int_int)((uintptr_t)dst | 1u);
}

/*
 * A run of `adds r0, #1` ending in `bx lr`, to time straight-line execution.
 *
 * Not NOPs: the core can fold those in the decoder and retire more than one per
 * cycle, which reports a rate the machine cannot actually sustain for real
 * work. `adds` has a register dependency chain and must occupy an execute slot.
 */
static fn_int_int build_add_sled(volatile uint16_t *dst, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
        dst[i] = 0x3001;                /* adds r0, #1 */
    dst[count] = 0x4770;                /* bx lr */
    __dmb();
    __isb();
    return (fn_int_int)((uintptr_t)dst | 1u);
}

static void test_exec(size_t psram_bytes)
{
    if (!psram_bytes) {
        printf("RESULT exec psram=0 skipped=1\n");
        return;
    }

    volatile uint16_t *code = (volatile uint16_t *)(XIP_PSRAM_BASE + CODE_OFFSET);

    fn_int_int inc = build_increment(code);
    int got = inc(41);
    printf("RESULT exec call ok=%u returned=%d\n", (unsigned)(got == 42), got);

    /*
     * Straight-line execution rate, measured twice.
     *
     * The first attempt at this used a 4096-instruction sled -- 8 KiB, which
     * fits inside the XIP cache -- and reported 500 MIPS at 252 MHz. Two
     * instructions per cycle on a single-issue M33 is not a measurement of
     * PSRAM, it is a measurement of the cache plus the core folding NOPs in the
     * decoder. Both effects have to go before the number means anything.
     *
     * So: a sled far larger than the cache, and `adds` rather than `nop`,
     * because a NOP can be discarded before it ever occupies an execute slot.
     */
    const uint32_t iters = 20;
    struct { const char *name; uint32_t count; } runs[] = {
        { "cached",   2u * 1024u },     /*  4 KiB -- fits the XIP cache      */
        { "streamed", 48u * 1024u },    /* 96 KiB -- cannot possibly fit     */
    };

    for (size_t r = 0; r < count_of(runs); r++) {
        fn_int_int sled = build_add_sled(code + 8, runs[r].count);
        sled(0);                        /* warm */
        absolute_time_t t0 = get_absolute_time();
        for (uint32_t i = 0; i < iters; i++)
            sled(0);
        int64_t us = absolute_time_diff_us(t0, get_absolute_time());

        uint64_t insn = (uint64_t)runs[r].count * iters;
        uint32_t mips = us > 0 ? (uint32_t)(insn / (uint64_t)us) : 0;
        /* Cycles per instruction x100, against the known clock. */
        uint32_t cpi100 = mips ? (uint32_t)((uint64_t)CPU_SPEED * 100u / mips) : 0;
        printf("RESULT exec sled=%-9s insns=%u us=%u mips=%u cpi=%u.%02u\n",
               runs[r].name, (unsigned)insn, (unsigned)us, (unsigned)mips,
               (unsigned)(cpi100 / 100), (unsigned)(cpi100 % 100));
    }
}

/*
 * Instruction-side coherence: overwrite a function and call it again.
 *
 * Without an ISB after the write this is allowed to fail even on a coherent
 * machine -- the pipeline may hold the old instruction. The question here is
 * whether the *memory system* needs more than that, i.e. whether the XIP cache
 * can serve a stale line to the fetch unit after a store.
 */
static void test_icoherence(size_t psram_bytes)
{
    if (!psram_bytes) {
        printf("RESULT icoh skipped=1\n");
        return;
    }

    volatile uint16_t *code = (volatile uint16_t *)(XIP_PSRAM_BASE + CODE_OFFSET + 0x100u);

    fn_int_int fn = build_increment(code);
    int first = fn(10);                 /* 11 */

    /* Rewrite the same address: adds r0, #2 */
    code[0] = 0x3002;
    __dmb();
    __isb();
    int second = fn(10);                /* 12 if the new code is visible */

    printf("RESULT icoh first=%d second=%d coherent=%u\n",
           first, second, (unsigned)(first == 11 && second == 12));
}

/*
 * Data-side coherence between the CPU and DMA, in PSRAM.
 *
 * Prime the CPU's view by reading, then have DMA overwrite the same bytes, then
 * read again. If the second read returns the DMA's data, CPU and DMA share a
 * view. Also checked through the uncached alias, which tells us whether any
 * incoherence is the XIP cache or something further down.
 */
static void test_dcoherence(size_t psram_bytes)
{
    if (!psram_bytes) {
        printf("RESULT dcoh skipped=1\n");
        return;
    }

    volatile uint32_t *cached =
        (volatile uint32_t *)(XIP_PSRAM_BASE + DATA_OFFSET);
    volatile uint32_t *uncached =
        (volatile uint32_t *)(XIP_PSRAM_NOCACHE_BASE + DATA_OFFSET);

    static uint32_t source[16];
    for (uint32_t i = 0; i < count_of(source); i++)
        source[i] = 0xC0DE0000u + i;

    for (uint32_t i = 0; i < count_of(source); i++)
        cached[i] = 0xEEEEEEEEu;
    __dmb();

    uint32_t primed = cached[0];        /* pull it into the XIP cache */

    int ch = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, true);
    dma_channel_configure(ch, &c, (void *)cached, source, count_of(source), true);
    dma_channel_wait_for_finish_blocking(ch);
    dma_channel_unclaim(ch);
    __dmb();

    uint32_t after_cached = cached[0];
    uint32_t after_uncached = uncached[0];

    printf("RESULT dcoh primed=0x%08x after_cached=0x%08x after_uncached=0x%08x "
           "cpu_sees_dma=%u\n",
           (unsigned)primed, (unsigned)after_cached, (unsigned)after_uncached,
           (unsigned)(after_cached == source[0]));
}

/* ---- interrupt latency, in cycles ------------------------------------ */
/*
 * The first version of this measured a timer alarm with the 1 us timer, and
 * reported "max 1 us idle, 0 us under load" for everything. At 252 MHz one
 * microsecond is 252 cycles, so that resolution cannot distinguish a clean
 * dispatch from one that stalled on an XIP miss -- exactly the difference the
 * kernel port needs to know.
 *
 * So: cycle counts from the DWT, and a software-triggered interrupt rather than
 * a timer, because pending-to-entry is a span we can bracket exactly.
 *
 * The comparison that decides the design is not idle-versus-loaded, it is
 * **handler in flash XIP versus handler in SRAM**. If a cold handler costs
 * meaningfully more, the kernel's exception entry and hot IRQ paths have to be
 * forced into SRAM; if not, that whole complication can be dropped.
 */

#define DWT_CTRL   (*(volatile uint32_t *)0xE0001000u)
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004u)
#define DEMCR      (*(volatile uint32_t *)0xE000EDFCu)

static void cyccnt_init(void)
{
    DEMCR |= (1u << 24);        /* TRCENA */
    DWT_CYCCNT = 0;
    DWT_CTRL |= 1u;             /* CYCCNTENA */
}

static volatile uint32_t sw_irq_entry_cycles;
static volatile uint32_t sw_irq_start_cycles;
static volatile uint32_t sw_irq_count;

/* Handler compiled into flash, executed via XIP: the default placement, and
 * what a kernel handler would be unless we take steps. */
static void sw_handler_flash(void)
{
    sw_irq_entry_cycles = DWT_CYCCNT - sw_irq_start_cycles;
    sw_irq_count++;
}

/* The same handler forced into SRAM. `__not_in_flash_func` is the pico-sdk's
 * way of saying "copy this to RAM at startup and run it from there". */
static void __not_in_flash_func(sw_handler_sram)(void)
{
    sw_irq_entry_cycles = DWT_CYCCNT - sw_irq_start_cycles;
    sw_irq_count++;
}

static void measure_dispatch(const char *label, void (*handler)(void),
                             const volatile uint32_t *thrash, uint32_t words)
{
    /* SPARE_IRQ_0 exists precisely so software can raise an interrupt that no
     * peripheral owns. */
    const uint irq = SPARE_IRQ_0;

    irq_set_exclusive_handler(irq, handler);
    irq_set_enabled(irq, true);

    uint32_t total = 0, worst = 0;
    const uint32_t rounds = 500;
    volatile uint32_t sink = 0;

    for (uint32_t r = 0; r < rounds; r++) {
        /* Leave outstanding XIP traffic in flight so the dispatch has to
         * contend with a stalled memory system, not an idle one. */
        if (thrash)
            for (uint32_t i = 0; i < words; i += 16)
                sink += thrash[i];

        sw_irq_count = 0;
        __dmb();
        sw_irq_start_cycles = DWT_CYCCNT;
        irq_set_pending(irq);
        while (!sw_irq_count)
            tight_loop_contents();

        uint32_t c = sw_irq_entry_cycles;
        total += c;
        if (c > worst)
            worst = c;
    }

    irq_set_enabled(irq, false);
    irq_remove_handler(irq, handler);

    printf("RESULT dispatch %-22s mean_cycles=%u worst_cycles=%u\n",
           label, (unsigned)(total / rounds), (unsigned)worst);
    (void)sink;
}

/* ---- timer-based latency (kept for the absolute figure) -------------- */

static volatile uint32_t irq_fired;
static volatile uint32_t irq_target_us;
static volatile uint32_t irq_latency_max;
static volatile uint64_t irq_latency_sum;

static void alarm_handler(void)
{
    uint32_t now = timer_hw->timerawl;
    hw_clear_bits(&timer_hw->intr, 1u << 0);

    uint32_t late = now - irq_target_us;
    if (late > irq_latency_max)
        irq_latency_max = late;
    irq_latency_sum += late;
    irq_fired++;
}

/*
 * Latency of a timer interrupt while the foreground streams from PSRAM.
 *
 * The alarm is armed for a fixed time ahead; the handler records how late it
 * actually ran. The foreground meanwhile walks a buffer far larger than the XIP
 * cache, so nearly every access misses and stalls the core on QMI. That is the
 * condition a kernel will spend its life in, and the number decides how much of
 * it has to live in SRAM.
 *
 * Resolution is 1 us -- the timer's tick -- so this measures the scale of the
 * problem, not single-cycle detail.
 */
static void test_irq_latency(size_t psram_bytes, const char *label,
                             const volatile uint32_t *stream, uint32_t words)
{
    irq_fired = 0;
    irq_latency_max = 0;
    irq_latency_sum = 0;

    hw_set_bits(&timer_hw->inte, 1u << 0);
    irq_set_exclusive_handler(TIMER0_IRQ_0, alarm_handler);
    irq_set_enabled(TIMER0_IRQ_0, true);

    const uint32_t rounds = 200;
    const uint32_t ahead_us = 200;
    volatile uint32_t sink = 0;

    for (uint32_t r = 0; r < rounds; r++) {
        uint32_t start = timer_hw->timerawl;
        irq_target_us = start + ahead_us;
        timer_hw->alarm[0] = irq_target_us;

        uint32_t before = irq_fired;
        /* Keep the core busy missing in the XIP cache until the alarm lands. */
        while (irq_fired == before) {
            if (stream) {
                for (uint32_t i = 0; i < words; i += 8)
                    sink += stream[i];
            }
        }
    }

    irq_set_enabled(TIMER0_IRQ_0, false);
    irq_remove_handler(TIMER0_IRQ_0, alarm_handler);
    hw_clear_bits(&timer_hw->inte, 1u << 0);

    printf("RESULT irqlat load=%-12s fired=%u mean_us=%u max_us=%u\n",
           label, (unsigned)irq_fired,
           (unsigned)(irq_fired ? irq_latency_sum / irq_fired : 0),
           (unsigned)irq_latency_max);
    (void)psram_bytes;
    (void)sink;
}

/*
 * Vector table relocation.
 *
 * The bootloader has to point VTOR at the kernel's table before handing over.
 * Confirm the table can be moved to SRAM and that an interrupt taken afterwards
 * dispatches through the new one. VTOR requires the table to be aligned to at
 * least its own size rounded up to a power of two; 256 entries * 4 bytes = 1 KiB.
 */
static uint32_t relocated_vectors[256] __attribute__((aligned(1024)));
static volatile uint32_t relocated_handler_ran;

static void relocated_alarm_handler(void)
{
    hw_clear_bits(&timer_hw->intr, 1u << 1);
    relocated_handler_ran++;
}

static void test_vtor(void)
{
    uint32_t original = scb_hw->vtor;

    memcpy(relocated_vectors, (const void *)(uintptr_t)original,
           sizeof(relocated_vectors));

    relocated_handler_ran = 0;
    /* TIMER0_IRQ_1 is exception number 16 + irq. */
    relocated_vectors[16 + TIMER0_IRQ_1] =
        (uint32_t)((uintptr_t)relocated_alarm_handler | 1u);

    __dmb();
    scb_hw->vtor = (uint32_t)(uintptr_t)relocated_vectors;
    __dsb();
    __isb();

    hw_set_bits(&timer_hw->inte, 1u << 1);
    irq_set_enabled(TIMER0_IRQ_1, true);
    timer_hw->alarm[1] = timer_hw->timerawl + 1000u;

    uint32_t spins = 0;
    while (!relocated_handler_ran && spins++ < 10000000u)
        tight_loop_contents();

    irq_set_enabled(TIMER0_IRQ_1, false);
    hw_clear_bits(&timer_hw->inte, 1u << 1);

    __dmb();
    scb_hw->vtor = original;
    __dsb();
    __isb();

    printf("RESULT vtor original=0x%08x relocated=0x%08x dispatched=%u\n",
           (unsigned)original, (unsigned)(uintptr_t)relocated_vectors,
           (unsigned)relocated_handler_ran);
}

/* Unaligned access. ARMv7-M and later permit it for plain loads and stores, and
 * the kernel relies on that in places; confirm it does not fault here. */
static void test_unaligned(size_t psram_bytes)
{
    static uint8_t buf[16];
    volatile uint32_t *p = (volatile uint32_t *)(buf + 1);
    *p = 0x11223344u;
    __dmb();
    unsigned sram_ok = (*p == 0x11223344u);

    unsigned psram_ok = 0;
    if (psram_bytes) {
        volatile uint32_t *q =
            (volatile uint32_t *)(XIP_PSRAM_BASE + DATA_OFFSET + 0x201u);
        *q = 0x55667788u;
        __dmb();
        psram_ok = (*q == 0x55667788u);
    }
    printf("RESULT unaligned sram=%u psram=%u\n", sram_ok, psram_ok);
}

/*
 * Re-derive the flash QSPI divider from the current system clock.
 *
 * boot2 programs QMI M0's divider once, at the boot clock, and it stays put.
 * The pico-sdk's default is CLKDIV 2, which at 252 MHz gives a 126 MHz QSPI
 * clock -- inside the W25Q128's rating. Raise the system clock to 504 MHz
 * without touching it and the same divider asks the flash for 252 MHz, which it
 * cannot do: XIP returns nonsense and the core dies on the next instruction
 * fetch, before it can print anything about why.
 *
 * That is exactly what happened on the first 504 MHz attempt here: no console
 * output at all, no fault message, nothing to read. (The FRANK firmware defines
 * FLASH_MAX_FREQ_MHZ but never uses it; it stays correct at 252 MHz by
 * accident of the default divider, not by calculation.)
 *
 * afboot-rp2350 will need this same routine before it can hand a kernel a clock
 * of its choosing, so it is worth getting right here.
 *
 * Must not execute from flash while it reprograms flash timing, hence
 * __no_inline_not_in_flash_func.
 */
#ifndef FLASH_MAX_FREQ_MHZ
#define FLASH_MAX_FREQ_MHZ 66
#endif

static uint32_t __no_inline_not_in_flash_func(flash_set_clkdiv)(uint32_t clock_hz)
{
    const uint32_t max_hz = FLASH_MAX_FREQ_MHZ * 1000000u;

    uint32_t divisor = (clock_hz + max_hz - 1u) / max_hz;
    if (divisor < 1u)
        divisor = 1u;
    if (divisor > 255u)
        divisor = 255u;

    /* RXDELAY has to grow with the divider: the further the sampling point is
     * from the launch edge in system clocks, the later the data comes back. */
    uint32_t rxdelay = divisor;
    if (clock_hz / divisor > 100000000u)
        rxdelay += 1u;

    uint32_t timing = qmi_hw->m[0].timing;
    timing &= ~(QMI_M0_TIMING_CLKDIV_BITS | QMI_M0_TIMING_RXDELAY_BITS);
    timing |= (divisor << QMI_M0_TIMING_CLKDIV_LSB)
            | (rxdelay << QMI_M0_TIMING_RXDELAY_LSB);

    qmi_hw->m[0].timing = timing;
    __dmb();
    /* Let the new timing settle before the next fetch from flash. */
    for (volatile int i = 0; i < 1000; i++)
        __asm__ volatile("nop");

    return divisor;
}

static size_t psram_probe_bytes(void)
{
    volatile uint32_t *base = (volatile uint32_t *)XIP_PSRAM_NOCACHE_BASE;
    const uint32_t marker = 0x5AA51234u;

    base[0] = marker;
    __dmb();
    if (base[0] != marker)
        return 0;

    for (size_t size = 1024u; size <= 16u * 1024u * 1024u; size <<= 1) {
        volatile uint32_t *probe =
            (volatile uint32_t *)(XIP_PSRAM_NOCACHE_BASE + size);
        uint32_t saved = *probe;
        *probe = ~marker;
        __dmb();
        bool aliased = (base[0] != marker);
        *probe = saved;
        __dmb();
        base[0] = marker;
        if (aliased)
            return size;
    }
    return 16u * 1024u * 1024u;
}

int main(void)
{
    /* The regulator exposes 1.50 / 1.60 / 1.65 / 1.70 V steps; the thresholds
     * match what the FRANK firmware uses, since that is what this board is
     * known to run at. 1.50 V will not hold 504 MHz. */
#if CPU_SPEED >= 300
    /* The regulator refuses anything above VREG_VOLTAGE_MAX -- which on RP2350
     * is 1.30 V -- unless the limit is disabled first. Without this call the
     * request is silently clamped: the chip comes up underpowered at the
     * requested clock and dies, having emitted a single garbage byte. That is
     * indistinguishable from a crash, and it is what the first 504 MHz attempts
     * here actually were. */
    vreg_disable_voltage_limit();
#endif
#if CPU_SPEED >= 504
    vreg_set_voltage(VREG_VOLTAGE_1_65);
#elif CPU_SPEED >= 300
    vreg_set_voltage(VREG_VOLTAGE_1_60);
#else
    vreg_set_voltage(VREG_VOLTAGE_1_50);
#endif
    sleep_ms(10);

    /* Set the flash divider for the clock we are ABOUT to run at, before
     * raising it -- not after.
     *
     * set_sys_clock_khz() returns into flash-resident code. Raise the clock
     * first and the very next instruction fetch happens at the new speed with
     * boot2's old divider still in place: at 504 MHz that asks the W25Q128 for
     * 252 MHz, XIP returns nonsense, and the core dies before it can reach any
     * code that would have fixed the divider. Which is exactly what the first
     * three 504 MHz attempts did -- one NUL byte and silence.
     *
     * Programming it first is safe in both directions: until the clock rises,
     * the flash simply runs slower than it needs to. */
    uint32_t flash_div = flash_set_clkdiv(CPU_SPEED * 1000000u);
    set_sys_clock_khz(CPU_SPEED * 1000, true);

    /* Pin clk_peri to the 48 MHz USB PLL before bringing up stdio.
     *
     * set_sys_clock_khz() repoints clk_peri at clk_sys, so the UART's divisor
     * would be derived from whatever the system clock happens to be. That is
     * survivable at 252 MHz and not at 504: the first 504 MHz run emitted a
     * single garbage byte and then nothing, which reads exactly like a crash
     * and is actually a baud rate. Protea hit the same thing from the other
     * direction -- its clk_peri ended up at 252 MHz and every rate came out
     * 5.25x fast (protea/firmware/rp2350/docs/dev.md).
     *
     * A fixed 48 MHz peripheral clock makes the console independent of the
     * overclock, which is what we want when the overclock is the thing under
     * test. */
    clock_configure(clk_peri, 0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                    48 * MHZ, 48 * MHZ);
    stdio_init_all();

    for (int i = 0; i < 6; i++) {
        printf("hwtest xip - waiting for console (%d/6)\n", i + 1);
        sleep_ms(200);
    }

    printf("\n=== RP2350 XIP execution, coherence and latency ===\n");
    printf("sys_clk %u MHz\n", (unsigned)(clock_get_hz(clk_sys) / 1000000u));
    printf("RESULT clocks sys_mhz=%u flash_div=%u flash_mhz=%u\n",
           (unsigned)(clock_get_hz(clk_sys) / 1000000u), (unsigned)flash_div,
           (unsigned)(clock_get_hz(clk_sys) / flash_div / 1000000u));

    psram_init(FRANK_PSRAM_CS_PIN);
    size_t psram_bytes = psram_probe_bytes();
    printf("RESULT psram bytes=%u\n", (unsigned)psram_bytes);

    test_exec(psram_bytes);
    test_icoherence(psram_bytes);
    test_dcoherence(psram_bytes);
    test_unaligned(psram_bytes);
    test_vtor();

    /* The decisive comparison: does a handler executing from flash XIP dispatch
     * measurably slower than one in SRAM? That is what decides whether the
     * kernel's exception entry has to be relocated. */
    cyccnt_init();
    measure_dispatch("flash_handler/idle", sw_handler_flash, NULL, 0);
    measure_dispatch("sram_handler/idle", sw_handler_sram, NULL, 0);
    if (psram_bytes) {
        measure_dispatch("flash_handler/psram_load", sw_handler_flash,
                         (const volatile uint32_t *)XIP_PSRAM_BASE,
                         256u * 1024u / 4u);
        measure_dispatch("sram_handler/psram_load", sw_handler_sram,
                         (const volatile uint32_t *)XIP_PSRAM_BASE,
                         256u * 1024u / 4u);
    }

    /* Idle first, as the control: any latency here is the timer and the NVIC,
     * not the memory system. Then the same measurement with the core thrashing
     * the XIP cache, which is the difference we actually care about. */
    test_irq_latency(psram_bytes, "idle", NULL, 0);
    if (psram_bytes) {
        test_irq_latency(psram_bytes, "psram_stream",
                         (const volatile uint32_t *)XIP_PSRAM_BASE,
                         256u * 1024u / 4u);
    }
    test_irq_latency(psram_bytes, "flash_stream",
                     (const volatile uint32_t *)XIP_FLASH_BASE,
                     256u * 1024u / 4u);

    printf("=== done ===\n");
    while (true)
        tight_loop_contents();
}
