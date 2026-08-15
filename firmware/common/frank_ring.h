/*
 * frank_ring.h - the SRAM mailbox between Linux on core 0 and the service
 *                firmware on core 1.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Linux cannot drive the RP2350's USB controller -- there is no mainline driver
 * for it and writing one is not a side quest. So core 1 runs TinyUSB as a CDC
 * device and Linux talks to core 1 instead, through two byte rings in SRAM.
 * From the kernel's point of view this is a trivial character device; all the
 * USB lives on the other core.
 *
 * The same structure carries the inter-chip link later, which is why the rings
 * are described here rather than inside the USB firmware.
 *
 *
 * Why there are no locks or atomics in here
 * -----------------------------------------
 * Each ring has exactly one producer and one consumer, and they are on
 * different cores and never swap roles. A single-producer/single-consumer ring
 * needs no mutual exclusion at all: the producer owns `head`, the consumer owns
 * `tail`, each only ever reads the other's index, and a 32-bit aligned load or
 * store on this machine is single-copy atomic on its own.
 *
 * That is not merely tidy, it is necessary. LDREX/STREX do not work outside
 * SRAM on this chip and the kernel therefore runs with interrupt-masked atomics
 * (see docs/atomics-port.md), which are only atomic against *this* core -- they
 * would not protect anything against core 1. Rings that need no atomics
 * sidestep the whole problem. SIO hardware spinlocks are the fallback if some
 * future structure genuinely needs cross-core mutual exclusion; they work in
 * every memory region, at about 75 ns (hw-findings F7).
 *
 * The ordering rules that remain, and they are not optional:
 *
 *   producer: write the data, DMB, then publish the new head
 *   consumer: read head, DMB, then read the data, DMB, then publish the new tail
 *
 * Without the barrier before publishing, the reader can observe a head that
 * promises bytes the writer has not actually stored yet.
 *
 *
 * Where it lives
 * --------------
 * In SRAM, and deliberately so. Linux's system RAM is the 8 MB of PSRAM at
 * 0x11000000; the DTS gives it no SRAM at all, so the whole 520 kB is core 1's
 * and nothing the kernel allocates can ever land on top of these rings. SRAM is
 * also the one region where exclusives work, and by far the fastest thing on
 * the chip -- an XIP miss costs about ten cycles per instruction (F9).
 */

#ifndef FRANK_RING_H
#define FRANK_RING_H

#include <stdint.h>

/* "FRNG". Checked by both sides at startup: if core 1 has not run, or the two
 * halves were built from different trees, this is what says so rather than
 * letting garbage indices drive a memcpy. */
#define FRANK_RING_MAGIC    0x474e5246u
#define FRANK_RING_VERSION  1u

/* Must be a power of two: the index arithmetic masks rather than divides. */
#define FRANK_RING_BYTES    2048u
#define FRANK_RING_MASK     (FRANK_RING_BYTES - 1u)

/*
 * Fixed address in SRAM, agreed by three separate builds -- the bootloader, the
 * core 1 firmware and the Linux driver -- so it cannot be a linker symbol.
 *
 * RP2350 SRAM is 0x20000000..0x20082000 (520 kB). This sits at the top, clear
 * of core 1's own code and stack which start from the bottom.
 */
#define FRANK_RING_BASE     0x20080000u

typedef struct {
    volatile uint32_t head;      /* producer writes, consumer reads  */
    volatile uint32_t tail;      /* consumer writes, producer reads  */
    volatile uint8_t  data[FRANK_RING_BYTES];
} frank_ring_t;

typedef struct {
    volatile uint32_t magic;
    volatile uint32_t version;
    volatile uint32_t core1_alive;   /* core 1 bumps this in its service loop */
    volatile uint32_t reserved;

    frank_ring_t tx;                 /* Linux -> core 1 -> USB host  */
    frank_ring_t rx;                 /* USB host -> core 1 -> Linux  */
} frank_ring_shared_t;

#define FRANK_RING_SHARED   ((frank_ring_shared_t *)FRANK_RING_BASE)

#ifndef __KERNEL__

static inline void frank_ring_barrier(void)
{
    __asm__ volatile("dmb" ::: "memory");
}

static inline uint32_t frank_ring_used(const frank_ring_t *r)
{
    return (r->head - r->tail) & FRANK_RING_MASK;
}

static inline uint32_t frank_ring_space(const frank_ring_t *r)
{
    /* One byte is always left unused so that head == tail means empty rather
     * than being ambiguous with completely full. */
    return FRANK_RING_MASK - frank_ring_used(r);
}

/* Producer side. Returns bytes actually written. */
static inline uint32_t frank_ring_put(frank_ring_t *r, const uint8_t *src,
                                      uint32_t len)
{
    uint32_t space = frank_ring_space(r);
    if (len > space)
        len = space;

    uint32_t head = r->head;
    for (uint32_t i = 0; i < len; i++)
        r->data[(head + i) & FRANK_RING_MASK] = src[i];

    /* Publish only after the bytes are visible. */
    frank_ring_barrier();
    r->head = (head + len) & FRANK_RING_MASK;
    return len;
}

/* Consumer side. Returns bytes actually read. */
static inline uint32_t frank_ring_get(frank_ring_t *r, uint8_t *dst,
                                      uint32_t len)
{
    uint32_t used = frank_ring_used(r);
    if (len > used)
        len = used;

    /* Read the data only after observing the head that promises it. */
    frank_ring_barrier();

    uint32_t tail = r->tail;
    for (uint32_t i = 0; i < len; i++)
        dst[i] = r->data[(tail + i) & FRANK_RING_MASK];

    frank_ring_barrier();
    r->tail = (tail + len) & FRANK_RING_MASK;
    return len;
}

#endif /* !__KERNEL__ */

#endif /* FRANK_RING_H */
