/*
 * Phase 2, item 1: do LDREX/STREX work outside SRAM on the RP2350?
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This is the go/no-go test for the whole ARM port.
 *
 * On the RISC-V side of this chip, atomics only work in SRAM. The datasheet is
 * explicit about it (MCAUSE code 7: "an AMO is attempted on a region that does
 * not support atomics (on RP2350, anything but SRAM)"), and pi-pico2-linux has
 * to trap every atomic instruction and emulate it -- its SC handler simply lies
 * and reports success unconditionally, because it has no other option.
 *
 * The ARM side should be different in kind, not degree. A Cortex-M33's *local*
 * exclusive monitor lives inside the core, not out on the bus, so exclusives on
 * Normal non-shareable memory ought to succeed wherever the memory happens to
 * be. If that holds, a NOMMU kernel with its data in PSRAM needs no emulation
 * at all. If it does not, the ARM port inherits the same problem and the plan
 * changes here rather than in Phase 4.
 *
 * A kernel does not care whether one STREX succeeds; it cares whether a retry
 * loop terminates. So the measurement is "how many attempts before the first
 * success", not "did an attempt succeed". An address where STREX never
 * succeeds does not fail loudly -- it livelocks inside cmpxchg forever.
 */

#include <stdio.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/structs/mpu.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#include "hardware/vreg.h"
#include "pico/stdlib.h"

#include "psram_init.h"

#ifndef CPU_SPEED
#define CPU_SPEED 252
#endif

/* XIP windows. CS0 is flash, CS1 is the PSRAM that psram_init() maps.
 * The 0x14000000/0x15000000 aliases bypass the XIP cache entirely, which is
 * what makes them worth testing separately: an exclusive that only works
 * because the cache absorbed it is not an exclusive. */
#define XIP_FLASH_BASE          0x10000000u
#define XIP_PSRAM_BASE          0x11000000u
#define XIP_PSRAM_NOCACHE_BASE  0x15000000u

#define SRAM_BASE               0x20000000u

/* How many times to retry a single exclusive before calling it dead. A real
 * kernel retry loop is unbounded; anything that needs more than a handful of
 * attempts with no contention is already broken. */
#define MAX_ATTEMPTS 1000

/* How many independent exclusive operations to run per region. One success
 * proves nothing -- a monitor that works once and then wedges is exactly the
 * failure mode that would survive a smaller sample. */
#define OPS_PER_REGION 1000

static uint32_t sram_target;

typedef struct {
    const char *name;
    volatile uint32_t *addr;
    bool writable;
} region_t;

/* Result of hammering one region. */
typedef struct {
    uint32_t ops_ok;          /* exclusives that eventually succeeded         */
    uint32_t ops_failed;      /* exclusives that gave up after MAX_ATTEMPTS   */
    uint32_t attempts_total;  /* summed attempts, to get a mean               */
    uint32_t attempts_worst;  /* worst single operation                       */
    uint32_t value_wrong;     /* the memory did not hold what we stored       */
    uint32_t plain_ok;        /* ordinary loads/stores work here at all       */
} excl_result_t;

/*
 * One LDREX/ADD/STREX attempt, as a single asm block. Returns 0 on success.
 *
 * Deliberately not the compiler's atomic builtins or separate ldrex()/strex()
 * helpers. The architecture allows almost anything between the two
 * instructions -- a spill, a call, an exception -- to clear the local monitor,
 * so a compiler free to insert code there could make a perfectly healthy region
 * look like it never grants an exclusive. Emitting the pair as one block means
 * a failure here is the hardware's answer and not the optimiser's.
 *
 * (GCC also has no __builtin_arm_ldrex; that spelling is Clang's.)
 */
static inline uint32_t exclusive_try_increment(volatile uint32_t *addr)
{
    uint32_t val, res;
    __asm__ volatile(
        "ldrex %[v], [%[p]]\n\t"
        "adds  %[v], %[v], #1\n\t"
        "strex %[r], %[v], [%[p]]\n\t"
        : [v] "=&r"(val), [r] "=&r"(res)
        : [p] "r"(addr)
        : "memory", "cc");
    return res;
}

static inline void exclusive_clear(void)
{
    __asm__ volatile("clrex" ::: "memory");
}

/* One exclusive increment, retried until it takes. */
static uint32_t exclusive_increment(volatile uint32_t *addr, bool *ok)
{
    for (uint32_t attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
        if (exclusive_try_increment(addr) == 0) {
            *ok = true;
            return attempt;
        }
    }
    *ok = false;
    return MAX_ATTEMPTS;
}

/*
 * Does ordinary, non-exclusive access work at this address?
 *
 * Runs before the exclusive test and gates its interpretation. "STREX never
 * succeeds here" and "this address is not writable memory at all" produce
 * identical exclusive results, and they call for completely different
 * responses -- one is a CPU/bus property we must design around, the other is a
 * broken PSRAM setup we must fix. Without this check the headline finding of
 * the whole phase would rest on an untested assumption.
 */
static bool plain_access_works(volatile uint32_t *addr)
{
    static const uint32_t patterns[] = {
        0x00000000u, 0xFFFFFFFFu, 0xA5A5A5A5u, 0x5A5A5A5Au, 0xDEADBEEFu,
    };
    for (size_t i = 0; i < count_of(patterns); i++) {
        *addr = patterns[i];
        __dmb();
        if (*addr != patterns[i])
            return false;
    }
    return true;
}

static void test_region(const region_t *r, excl_result_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!r->writable)
        return;

    out->plain_ok = plain_access_works(r->addr) ? 1u : 0u;

    *r->addr = 0;
    __dmb();

    for (uint32_t i = 0; i < OPS_PER_REGION; i++) {
        bool ok = false;
        uint32_t attempts = exclusive_increment(r->addr, &ok);

        out->attempts_total += attempts;
        if (attempts > out->attempts_worst)
            out->attempts_worst = attempts;
        if (ok)
            out->ops_ok++;
        else
            out->ops_failed++;
    }

    __dmb();
    /* The counter must equal the number of successful increments. A monitor
     * that reports success without storing -- which is what the RISC-V
     * emulation does deliberately -- shows up here and nowhere else. */
    if (*r->addr != out->ops_ok)
        out->value_wrong = 1;
}

/*
 * Does an interrupt between LDREX and STREX clear the monitor?
 *
 * It must. Linux relies on it: an exception return executes CLREX, so a context
 * switch in the middle of a cmpxchg has to make the STREX fail rather than let
 * it commit against a stale read. A monitor that ignores interrupts is worse
 * than one that never succeeds, because it corrupts silently instead of hanging.
 */
static bool interrupt_clears_monitor(volatile uint32_t *addr)
{
    *addr = 0;
    __dmb();

    uint32_t val, res;
    /* CLREX is what an exception return performs, so issuing it explicitly
     * tests the same thing a context switch would do, without needing to
     * arrange a real interrupt to land in the window. */
    __asm__ volatile(
        "ldrex %[v], [%[p]]\n\t"
        "clrex\n\t"
        "adds  %[v], %[v], #1\n\t"
        "strex %[r], %[v], [%[p]]\n\t"
        : [v] "=&r"(val), [r] "=&r"(res)
        : [p] "r"(addr)
        : "memory", "cc");
    return res != 0;
}

/*
 * The mitigation, measured: interrupt-masked atomics.
 *
 * With no exclusive monitor outside SRAM, the kernel cannot use LDREX/STREX on
 * anything in PSRAM. The replacement is what Linux already does on ARMv5 --
 * mask interrupts around a plain read-modify-write. On a uniprocessor that is
 * genuinely atomic: interrupts off means nothing can preempt the sequence, and
 * CONFIG_SMP=n means there is no second CPU to race against.
 *
 * Worth measuring rather than assuming, on two counts. It has to actually
 * produce the right count in PSRAM, and it has to not be ruinously slow --
 * every atomic_inc in the kernel becomes a CPSID/load/store/CPSIE, executed
 * from memory that stalls on XIP misses.
 */
static uint32_t irq_masked_increment(volatile uint32_t *addr)
{
    uint32_t state = save_and_disable_interrupts();
    uint32_t v = *addr + 1u;
    *addr = v;
    restore_interrupts(state);
    return v;
}

/* ns per operation, and whether the final count is right. */
static void measure_irq_atomic(const char *name, volatile uint32_t *addr)
{
    const uint32_t n = 100000u;

    *addr = 0;
    __dmb();

    absolute_time_t t0 = get_absolute_time();
    for (uint32_t i = 0; i < n; i++)
        irq_masked_increment(addr);
    int64_t us = absolute_time_diff_us(t0, get_absolute_time());

    __dmb();
    printf("RESULT irqatomic region=%-14s ops=%u final=%u correct=%u ns_per_op=%u\n",
           name, (unsigned)n, (unsigned)*addr, (unsigned)(*addr == n),
           (unsigned)(us > 0 ? (uint64_t)us * 1000u / n : 0));
}

/*
 * The other option the silicon offers: SIO hardware spinlocks.
 *
 * These live in the SIO block, not in the memory being guarded, so they work
 * regardless of which region the data is in -- and unlike interrupt masking
 * they are also correct between the two cores. That matters for the core1 link
 * service, which shares rings with the kernel. Measured here so the cost is
 * known before anything depends on it.
 */
static void measure_spinlock_atomic(const char *name, volatile uint32_t *addr)
{
    const uint32_t n = 100000u;
    spin_lock_t *lock = spin_lock_instance(spin_lock_claim_unused(true));

    *addr = 0;
    __dmb();

    absolute_time_t t0 = get_absolute_time();
    for (uint32_t i = 0; i < n; i++) {
        uint32_t save = spin_lock_blocking(lock);
        *addr = *addr + 1u;
        spin_unlock(lock, save);
    }
    int64_t us = absolute_time_diff_us(t0, get_absolute_time());

    __dmb();
    printf("RESULT spinatomic region=%-14s ops=%u final=%u correct=%u ns_per_op=%u\n",
           name, (unsigned)n, (unsigned)*addr, (unsigned)(*addr == n),
           (unsigned)(us > 0 ? (uint64_t)us * 1000u / n : 0));
}

static uint32_t measure_read_kib_s(const volatile uint32_t *base, uint32_t bytes)
{
    absolute_time_t t0 = get_absolute_time();
    uint32_t acc = 0;
    for (uint32_t i = 0; i < bytes / 4u; i++)
        acc += base[i];
    int64_t us = absolute_time_diff_us(t0, get_absolute_time());
    __asm__ volatile("" :: "r"(acc));       /* keep the loop */
    return us > 0 ? (uint32_t)(((uint64_t)bytes * 1000000u) / (uint64_t)us / 1024u) : 0;
}

/*
 * PSRAM size by address aliasing.
 *
 * psram_init() maps the chip but does not report its size, so probe it: write a
 * marker at offset 0 and look for it repeating at each power-of-two offset. A
 * chip smaller than the window aliases, so the first offset that reads back the
 * marker is the size.
 *
 * Deliberately through the *uncached* alias. Through the cached window a write
 * followed by a read of the same address is answered out of the XIP cache, so a
 * missing chip looks present and an aliasing address looks like it is not
 * aliasing -- the probe would confirm whatever it was hoping for.
 */
static size_t psram_probe_bytes(void)
{
    volatile uint32_t *base = (volatile uint32_t *)XIP_PSRAM_NOCACHE_BASE;
    const uint32_t marker = 0x5AA51234u;

    base[0] = marker;
    __dmb();
    if (base[0] != marker)
        return 0;                       /* nothing answering at all */

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

static void report_mpu(void)
{
    uint32_t type = mpu_hw->type;
    uint32_t dregion = (type >> 8) & 0xFFu;
    /* PMSAv8 reports a unified MPU: no separate instruction regions, so the
     * SEPARATE bit is 0 and IREGION is 0. Linux's pmsa-v8.c expects exactly
     * that shape, and CONFIG_ARM_MPU builds pmsa-v7.o and pmsa-v8.o together
     * and picks at runtime. */
    printf("RESULT mpu type=0x%08x dregion=%u separate=%u\n",
           (unsigned)type, (unsigned)dregion, (unsigned)(type & 1u));
}

int main(void)
{
    vreg_set_voltage(VREG_VOLTAGE_1_50);
    sleep_ms(10);
    set_sys_clock_khz(CPU_SPEED * 1000, true);
    stdio_init_all();

    /* The console is UART0 on J2, read through the probe. Repeat the banner:
     * opening a terminal reliably takes longer than this test takes to run,
     * and a result you missed is a result you do not have. */
    for (int i = 0; i < 6; i++) {
        printf("hwtest exclusives - waiting for console (%d/6)\n", i + 1);
        sleep_ms(200);
    }

    printf("\n=== RP2350 exclusive monitor test ===\n");
    printf("sys_clk %u MHz\n", (unsigned)(clock_get_hz(clk_sys) / 1000000u));

    psram_init(FRANK_PSRAM_CS_PIN);
    size_t psram_bytes = psram_probe_bytes();
    printf("RESULT psram bytes=%u\n", (unsigned)psram_bytes);
    report_mpu();

    if (psram_bytes == 0)
        printf("WARN psram not detected -- PSRAM rows below are meaningless\n");

    /* Offset into each region so we are not testing address zero, which is
     * where an aliasing or unmapped window most easily looks like it works. */
    const region_t regions[] = {
        { "sram",           (volatile uint32_t *)&sram_target,                  true  },
        { "psram_cached",   (volatile uint32_t *)(XIP_PSRAM_BASE + 0x1000u),    psram_bytes > 0 },
        { "psram_uncached", (volatile uint32_t *)(XIP_PSRAM_NOCACHE_BASE + 0x2000u), psram_bytes > 0 },
    };

    for (size_t i = 0; i < count_of(regions); i++) {
        excl_result_t r;
        test_region(&regions[i], &r);
        printf("RESULT excl region=%-14s plain_ok=%u ok=%u failed=%u "
               "mean_attempts=%u worst=%u value_wrong=%u\n",
               regions[i].name, (unsigned)r.plain_ok,
               (unsigned)r.ops_ok, (unsigned)r.ops_failed,
               (unsigned)(r.ops_ok ? r.attempts_total / (r.ops_ok + r.ops_failed) : 0),
               (unsigned)r.attempts_worst, (unsigned)r.value_wrong);
    }

    printf("RESULT clrex sram=%u psram=%u\n",
           (unsigned)interrupt_clears_monitor((volatile uint32_t *)&sram_target),
           (unsigned)(psram_bytes ? interrupt_clears_monitor(
               (volatile uint32_t *)(XIP_PSRAM_BASE + 0x3000u)) : 0u));

    /* Validate the replacement for exclusives, in the region that needs it. */
    measure_irq_atomic("sram", (volatile uint32_t *)&sram_target);
    if (psram_bytes) {
        measure_irq_atomic("psram_cached",
                           (volatile uint32_t *)(XIP_PSRAM_BASE + 0x4000u));
        measure_spinlock_atomic("psram_cached",
                                (volatile uint32_t *)(XIP_PSRAM_BASE + 0x5000u));
    }
    measure_spinlock_atomic("sram", (volatile uint32_t *)&sram_target);

    /* Bandwidth, for the record: the plan's whole case for putting Linux on the
     * slave rests on PSRAM read rate, and it should be stated in the same
     * report as the atomics result. */
    printf("RESULT bandwidth flash_kib_s=%u psram_cached_kib_s=%u "
           "psram_uncached_kib_s=%u\n",
           (unsigned)measure_read_kib_s(
               (const volatile uint32_t *)XIP_FLASH_BASE, 256u * 1024u),
           (unsigned)(psram_bytes ? measure_read_kib_s(
               (const volatile uint32_t *)XIP_PSRAM_BASE, 256u * 1024u) : 0u),
           (unsigned)(psram_bytes ? measure_read_kib_s(
               (const volatile uint32_t *)XIP_PSRAM_NOCACHE_BASE, 64u * 1024u) : 0u));

    printf("=== done ===\n");
    while (true)
        tight_loop_contents();
}
