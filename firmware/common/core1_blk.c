/*
 * core1_blk.c - block requests from Linux, executed over the link.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Linux submits a descriptor in SRAM and bumps a sequence number; core 1 turns
 * that into a transaction with the master, which owns the microSD. The kernel
 * never touches PIO, DMA or the wire -- the same division that keeps USB out of
 * the kernel.
 *
 * Core 1 reads and writes the kernel's buffer directly rather than copying
 * through a ring: PSRAM is visible to both cores and CPU/DMA coherence there is
 * confirmed (hw-findings F9), so a 4 kB request costs one link transfer instead
 * of two memory copies plus a link transfer.
 */

#include <string.h>

#include "pico/stdlib.h"

#include "frank_ring.h"
#include "link_blk.h"
#include "link_bus.h"

/*
 * Link pins, slave end. Bus B carries slave -> master and bus A the other way,
 * which is the mirror of the master's assignment.
 */
#define LINK_TX_BASE   11
#define LINK_RX_BASE   1
#define LINK_DB_OUT    23      /* DB_SM: this end raises it to ask */
#define LINK_DB_IN     22      /* DB_MS: the master answers with it */
#define LINK_FS        21

static link_t blk_link;
static bool blk_link_up;

/*
 * One transaction. Returns 0 or a negative errno.
 *
 * Timeouts are generous. F3 measured the master's control round-trip at 141 us
 * idle but 2753 us once the USB HID host is running, and Phase 6 will turn that
 * on -- a timeout tight enough to look reasonable today would start failing the
 * moment a keyboard is plugged in.
 */
static int32_t blk_transact(uint32_t op, uint32_t lba, uint32_t count,
                            void *buf, uint32_t *capacity_out)
{
    link_blk_req_t req = {
        .magic = LINK_BLK_MAGIC,
        .op    = op,
        .lba   = lba,
        .count = count,
    };
    link_blk_rsp_t rsp = { .status = -5, .capacity = 0 };
    uint32_t bytes = count * LINK_BLK_SECTOR;

    if (!blk_link_up)
        return -19;                                     /* -ENODEV */

    /* Raise our doorbell and wait for the master to acknowledge with its own.
     * The master polls DB_SM for exactly this. */
    link_db_set(&blk_link, true);
    if (!link_db_wait(&blk_link, true, 5000000)) {
        link_db_set(&blk_link, false);
        return -110;                                    /* -ETIMEDOUT */
    }

    link_use_ctrl_rate(&blk_link);
    link_tx_start(&blk_link, &req, sizeof(req));
    if (!link_tx_finish(&blk_link, 1000000))
        goto fail;

    if (op == LINK_BLK_OP_WRITE) {
        link_tx_start(&blk_link, buf, bytes);
        if (!link_tx_finish(&blk_link, 3000000))
            goto fail;
    }

    link_rx_arm(&blk_link, &rsp, sizeof(rsp));
    if (link_rx_wait(&blk_link, 3000000) != 0)
        goto fail;

    if (rsp.status == 0 && op == LINK_BLK_OP_READ) {
        link_rx_arm(&blk_link, buf, bytes);
        if (link_rx_wait(&blk_link, 3000000) != 0)
            goto fail;
    }

    if (capacity_out)
        *capacity_out = rsp.capacity;

    link_db_set(&blk_link, false);
    link_db_wait(&blk_link, false, 1000000);
    return rsp.status;

fail:
    link_db_set(&blk_link, false);
    link_db_wait(&blk_link, false, 1000000);
    return -5;                                          /* -EIO */
}

void frank_blk_init(void)
{
    frank_ring_shared_t *s = FRANK_RING_SHARED;
    uint32_t capacity = 0;

    link_init(&blk_link, pio0, LINK_TX_BASE, LINK_RX_BASE,
              LINK_DB_OUT, LINK_DB_IN, LINK_FS, false);
    blk_link_up = true;

    s->blk.seq = 0;
    s->blk.done = 0;
    s->blk.status = 0;

    /* Ask the master how big the device is. If it does not answer, publish a
     * capacity of zero and let the kernel driver decline to register rather
     * than expose a disk that cannot be read. */
    if (blk_transact(LINK_BLK_OP_INFO, 0, 0, NULL, &capacity) != 0)
        capacity = 0;

    frank_ring_barrier();
    s->blk.capacity = capacity;
}

/*
 * Called from core 1's service loop. Polls rather than takes an interrupt:
 * there is no cross-core doorbell wired for this, and core 1 has nothing else
 * to do between USB passes.
 */
void frank_blk_service(void)
{
    frank_ring_shared_t *s = FRANK_RING_SHARED;
    uint32_t seq = s->blk.seq;

    if (seq == s->blk.done)
        return;

    /* Read the descriptor only after observing the sequence that publishes it. */
    frank_ring_barrier();

    uint32_t op    = s->blk.op;
    uint32_t lba   = s->blk.lba;
    uint32_t count = s->blk.count;
    void    *buf   = (void *)(uintptr_t)s->blk.addr;

    int32_t status = (count == 0 || count > LINK_BLK_MAX_SECTORS)
                     ? -22 : blk_transact(op, lba, count, buf, NULL);

    s->blk.status = status;
    /* Publish completion last: `done` is what the kernel polls, and `status`
     * must already be visible when it observes it. */
    frank_ring_barrier();
    s->blk.done = seq;
}
