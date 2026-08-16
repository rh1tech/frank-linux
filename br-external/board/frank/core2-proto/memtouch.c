/*
 * memtouch - can a user process use every page the kernel gives it?
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * On NOMMU the allocator hands a process real memory, and free_initmem() puts
 * the kernel's discarded __init section straight back into the same pool. If
 * any of what a process receives sits inside an MPU region that denies user
 * access, it faults on its own heap -- with nothing in the fault to say the
 * kernel handed it out.
 *
 * Allocate a block, write one word to every page, report the range, and repeat.
 *
 * With "keep" the blocks are not freed, which is the mode that matters: freeing
 * each one means the allocator hands back the same block every time and the
 * test never looks at any other memory. Keeping them walks down the free lists
 * until memory runs out, which is what reaches the pages that used to be the
 * kernel's __init section.
 *
 * Every local read after siglongjmp is volatile. Anything else has an
 * indeterminate value there, and an earlier version printed the previous
 * iteration's pointer, which made the results impossible to trust.
 */

#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#define PAGE 4096u

static sigjmp_buf escape;
static volatile unsigned long fault_at;

static void on_fault(int sig)
{
    siglongjmp(escape, sig);
}

int main(int argc, char **argv)
{
    unsigned long block = (argc > 1) ? strtoul(argv[1], NULL, 0) : 64u * 1024u;
    unsigned long count = (argc > 2) ? strtoul(argv[2], NULL, 0) : 16u;
    int keep = (argc > 3 && argv[3][0] == 'k');
    volatile unsigned long i;
    volatile int faults = 0, allocs = 0;

    signal(SIGSEGV, on_fault);
    signal(SIGBUS, on_fault);

    for (i = 0; i < count; i++) {
        volatile char * volatile p = malloc(block);
        volatile unsigned long off;
        int sig;

        if (!p) {
            printf("MEMTOUCH block %lu: malloc(%lu) failed after %d ok\n",
                   i, block, allocs);
            break;
        }
        allocs++;

        sig = sigsetjmp(escape, 1);
        if (sig == 0) {
            for (off = 0; off < block; off += PAGE) {
                fault_at = (unsigned long)(p + off);
                *(volatile unsigned long *)(p + off) = 0x5a5a5a5aUL;
            }
            printf("MEMTOUCH block %lu 0x%08lx +%lu OK\n",
                   i, (unsigned long)p, block);
        } else {
            printf("MEMTOUCH block %lu 0x%08lx +%lu FAULTED at 0x%08lx "
                   "(offset %lu) sig=%d\n",
                   i, (unsigned long)p, block, fault_at,
                   fault_at - (unsigned long)p, sig);
            faults++;
        }

        if (!keep)
            free((void *)p);
    }

    printf("MEMTOUCH RESULT %s (%d faults in %d blocks)\n",
           faults ? "UNUSABLE-MEMORY" : "all usable", faults, allocs);
    return faults ? 1 : 0;
}
