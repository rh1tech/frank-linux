/*
 * mputest - does the MPU actually keep userspace out of the kernel?
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * On NOMMU without an MPU there is nothing between a user process and kernel
 * memory: a stray pointer reads or writes it and nothing objects. That is the
 * single largest thing PMSAv8 buys on this board, and "the MPU initialised"
 * does not demonstrate it -- an MPU can be enabled and still map everything to
 * everyone. The only convincing evidence is a process being stopped.
 *
 * So this reads two addresses and reports what happened to each:
 *
 *   a local variable   -- must succeed, or the test itself is broken and a
 *                         fault below would prove nothing
 *   a kernel address   -- must fault
 *
 * The kernel address is not guessed. /proc/kallsyms would be ideal but is not
 * built in; instead the caller passes one, and tools/test-mpu.sh takes it from
 * the region the kernel programmed, read back over SWD.
 *
 * SIGSEGV is caught rather than fatal because a handler that simply returns
 * would re-run the faulting instruction forever -- siglongjmp is the way out.
 */

#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

static sigjmp_buf escape;

static void on_fault(int sig)
{
    siglongjmp(escape, sig);
}

/* Returns 1 if the read faulted, 0 if it succeeded. */
static int probe(const char *what, unsigned long addr)
{
    volatile unsigned int value;
    int sig;

    signal(SIGSEGV, on_fault);
    signal(SIGBUS, on_fault);

    sig = sigsetjmp(escape, 1);
    if (sig == 0) {
        value = *(volatile unsigned int *)addr;
        printf("MPUTEST %s 0x%08lx read 0x%08x OK\n", what, addr, value);
        return 0;
    }

    printf("MPUTEST %s 0x%08lx FAULTED sig=%d\n", what, addr, sig);
    return 1;
}

int main(int argc, char **argv)
{
    unsigned long kaddr;
    volatile unsigned int here = 0xa5a5a5a5u;
    int own_faulted, kernel_faulted;

    if (argc != 2) {
        fprintf(stderr, "usage: mputest <kernel-address>\n");
        return 2;
    }
    kaddr = strtoul(argv[1], NULL, 0);

    /* Control: reading our own stack must work. If this faults, the harness
     * below is meaningless -- a test that fails on everything proves nothing
     * about the thing it is supposed to be testing. */
    own_faulted = probe("own", (unsigned long)&here);

    kernel_faulted = probe("kernel", kaddr);

    if (own_faulted) {
        printf("MPUTEST RESULT broken -- could not read its own stack\n");
        return 2;
    }
    printf("MPUTEST RESULT %s\n", kernel_faulted ? "protected" : "UNPROTECTED");
    return kernel_faulted ? 0 : 1;
}
