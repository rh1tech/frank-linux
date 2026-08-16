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

/* Diagnostics for the one transaction that happens before Linux exists. Core 1
 * has no other way to say what went wrong, and "capacity 0" on the kernel side
 * is the same answer for every possible failure. */
static size_t blk_rx_remaining;
static int32_t blk_init_status;

void frank_ring_puts(const char *s);

static void blk_report(const char *what, int32_t v)
{
    char buf[16];
    int i = 15;
    bool neg = v < 0;
    uint32_t u = neg ? (uint32_t)(-v) : (uint32_t)v;

    buf[i--] = 0;
    if (!u) buf[i--] = '0';
    while (u && i > 0) { buf[i--] = (char)('0' + u % 10u); u /= 10u; }
    if (neg && i > 0) buf[i--] = '-';

    frank_ring_puts(what);
    frank_ring_puts(&buf[i + 1]);
    frank_ring_puts("\r\n");
}

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
    uint32_t bytes = count * LINK_BLK_SECTOR;

    /*
     * Staging buffer for the reply, which is the status header immediately
     * followed by any read data, as a single transfer.
     *
     * The receiver has to be armed before the sender starts: link_rx_arm()
     * restarts the RX state machine, which resets the input shift counter, and
     * that is what keeps 32-bit autopush words aligned with the transmitter's
     * words. Arming *after* sending the request -- the obvious order -- loses
     * the race whenever the master answers quickly, which for a request it can
     * satisfy from memory is always. The master saw and served the request, and
     * the reply went nowhere.
     *
     * Sending the header and the data as one transfer removes the second race
     * for free: with two transfers the slave would have to re-arm between them
     * and the master would have to somehow know when.
     */
    static uint8_t reply[sizeof(link_blk_rsp_t)
                         + LINK_BLK_MAX_SECTORS * LINK_BLK_SECTOR]
                   __attribute__((aligned(4)));
    link_blk_rsp_t rsp = { .status = -5, .capacity = 0 };
    uint32_t reply_bytes = sizeof(rsp) + ((op == LINK_BLK_OP_READ) ? bytes : 0);

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

    /* Armed first, before a single byte of the request goes out. */
    link_rx_arm(&blk_link, reply, reply_bytes);

    link_tx_start(&blk_link, &req, sizeof(req));
    if (!link_tx_finish(&blk_link, 1000000))
        goto fail;

    if (op == LINK_BLK_OP_WRITE) {
        link_tx_start(&blk_link, buf, bytes);
        if (!link_tx_finish(&blk_link, 3000000))
            goto fail;
    }

    blk_rx_remaining = link_rx_wait(&blk_link, 3000000);
    if (blk_rx_remaining != 0)
        goto fail;

    memcpy(&rsp, reply, sizeof(rsp));
    if (rsp.status == 0 && op == LINK_BLK_OP_READ)
        memcpy(buf, reply + sizeof(rsp), bytes);

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

/*
 * Set up the link only. The capacity probe deliberately does NOT happen here.
 *
 * frank_blk_init() is called before core 1 enters its service loop, so anything
 * slow here runs with tud_task() not being called -- and a link transaction
 * that finds the master absent blocks for eight seconds. USB cannot enumerate
 * during that, the host gives up, and the console never appears: measured as
 * core1_alive stuck at 0 with no CDC device at all.
 *
 * So the probe moves into the service loop, once USB is up. afboot waits for a
 * terminal before starting Linux, which is far longer than this needs.
 */
void frank_blk_init(void)
{
    frank_ring_shared_t *s = FRANK_RING_SHARED;

    link_init(&blk_link, pio0, LINK_TX_BASE, LINK_RX_BASE,
              LINK_DB_OUT, LINK_DB_IN, LINK_FS, false);
    blk_link_up = true;

    s->blk.seq = 0;
    s->blk.done = 0;
    s->blk.status = 0;
    s->blk.capacity = 0;
}

/* Ask the master how big the device is, once, after USB is serving. A capacity
 * of zero makes the kernel driver decline to register, which is better than
 * exposing a disk that cannot be read. */
static void blk_probe_once(void)
{
    frank_ring_shared_t *s = FRANK_RING_SHARED;
    uint32_t capacity = 0;

    blk_init_status = blk_transact(LINK_BLK_OP_INFO, 0, 0, NULL, &capacity);
    if (blk_init_status != 0)
        capacity = 0;

    blk_report("blk: info status=", blk_init_status);
    blk_report("blk: rx_remaining=", (int32_t)blk_rx_remaining);
    blk_report("blk: capacity=", (int32_t)capacity);

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
    static bool probed;
    static uint32_t warmup;
    uint32_t seq;

    /* Let USB enumerate before spending seconds on the link. */
    if (!probed) {
        if (++warmup < 200000u)
            return;
        probed = true;
        blk_probe_once();
        return;
    }

    seq = s->blk.seq;

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
