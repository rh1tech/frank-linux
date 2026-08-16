/*
 * castest - does the kernel's compare-and-exchange work, and is it atomic?
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * LDREX/STREX have no exclusive monitor covering PSRAM on this chip (F6), so
 * userspace cannot compare-and-exchange for itself and the C library asks the
 * kernel instead (patch 0010, and br-external/patches/uclibc/0002). Everything
 * with a lock in it now depends on that syscall, which makes it worth testing
 * on its own rather than only through the thing it was added for -- a fault
 * here would otherwise show up as "threads do not work", which is where this
 * started and is a much harder place to debug from.
 *
 * The syscall is called directly rather than through the library, so this tests
 * the kernel even when built against a libc that does not use it yet. That is
 * the point: it has to run on the image that exists before the toolchain is
 * rebuilt, which is the only way to find out whether the kernel half is right
 * before committing an hour to the other half.
 *
 * What is checked:
 *
 *   swap        it replaces the value when the word matches
 *   no-swap     it leaves the value alone when it does not, and says so
 *   report      the return value distinguishes the two
 *   wild-ptr    a bad pointer does not take the machine down
 *   atomicity   a counter incremented only through cmpxchg reaches its target
 *
 * The last is the interesting one. LDREX/STREX here fail silently and may
 * store while reporting failure (F6), so "the value came out right" is exactly
 * what a broken primitive cannot produce over many iterations.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* __ARM_NR_BASE + 0x00fff0. Spelled out because the installed kernel headers
 * are older than the patch that adds it. */
#define ARM_NR_CMPXCHG 0x0ffff0

static int failures;

static void check(const char *what, int ok, const char *detail)
{
    printf("CASTEST %-10s %s%s%s\n", what, ok ? "ok" : "FAIL",
           detail && *detail ? " -- " : "", detail ? detail : "");
    if (!ok)
        failures++;
}

/*
 * Returns 0 if *ptr was oldval and is now newval, 1 if it was not, or a
 * negative errno. Registers are pinned because this is the kernel's ABI for
 * the call: r0 expected, r1 new, r2 pointer, r7 the syscall number.
 */
static int kernel_cas(unsigned int oldval, unsigned int newval,
                      volatile unsigned int *ptr)
{
    register unsigned int r0 __asm__("r0") = oldval;
    register unsigned int r1 __asm__("r1") = newval;
    register volatile unsigned int *r2 __asm__("r2") = ptr;
    register unsigned int r7 __asm__("r7") = ARM_NR_CMPXCHG;
    register int res __asm__("r0");

    __asm__ __volatile__("svc\t#0"
                         : "=r"(res)
                         : "r"(r0), "r"(r1), "r"(r2), "r"(r7)
                         : "memory", "cc");
    return res;
}

int main(int argc, char **argv)
{
    volatile unsigned int word = 0x11111111u;
    unsigned long rounds = (argc > 1) ? strtoul(argv[1], NULL, 0) : 100000u;
    unsigned long i;
    char detail[96];
    int r;

    /* Does it exist at all? An unimplemented private syscall comes back as
     * -ENOSYS rather than a signal, which is the one failure worth naming
     * separately: it means the kernel is older than the patch. */
    r = kernel_cas(0x11111111u, 0x22222222u, &word);
    if (r == -ENOSYS) {
        printf("CASTEST RESULT no syscall -- kernel lacks __ARM_NR_cmpxchg\n");
        return 2;
    }

    snprintf(detail, sizeof detail, "returned %d, word now 0x%08x", r, word);
    check("swap", r == 0 && word == 0x22222222u, detail);

    r = kernel_cas(0xdeadbeefu, 0x33333333u, &word);
    snprintf(detail, sizeof detail, "returned %d, word now 0x%08x", r, word);
    check("no-swap", r == 1 && word == 0x22222222u, detail);

    /* A pointer no process owns.
     *
     * The check is that the machine is still here afterwards, not that the
     * call returns -EFAULT. It cannot: the kernel reads this with interrupts
     * masked and a fault taken there is not survivable (patch 0006), but the
     * MPU gives PL1 the whole address space -- eight regions with no gap the
     * kernel can reach -- so an access from kernel mode never faults at all.
     * It reads whatever is there and reports "did not match", which is the
     * truth.
     *
     * Worth testing anyway. What would be dangerous is the call taking the
     * machine down, and that is what this rules out. */
    r = kernel_cas(0, 1, (volatile unsigned int *)0x4);
    snprintf(detail, sizeof detail, "returned %d, still running", r);
    check("wild-ptr", r == 1 || r == -EFAULT, detail);

    /* Atomicity, or at least the part of it a single process can show: a
     * counter advanced only through the call has to arrive exactly. A
     * primitive that stores while reporting failure -- which is what STREX
     * does here -- overshoots or stalls. */
    word = 0;
    for (i = 0; i < rounds; i++) {
        unsigned int seen;

        do {
            seen = word;
        } while (kernel_cas(seen, seen + 1, &word) != 0);
    }
    snprintf(detail, sizeof detail, "%lu increments -> %u", rounds, word);
    check("counter", word == (unsigned int)rounds, detail);

    printf("CASTEST RESULT %s\n", failures ? "BROKEN" : "working");
    return failures ? 1 : 0;
}
