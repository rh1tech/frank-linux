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
 * The link stays quiet until core 0 says the machine is Linux's.
 *
 * Everything the link does -- the console to the master's terminal, the disk --
 * is for Linux. While afboot is still running it has no work to do, and doing
 * it anyway is not free: afboot copies 2.79 MB from flash XIP into PSRAM XIP,
 * and core 1 hammering the link through the same QMI at the same time wedged
 * core 0 hard enough that SWD could no longer examine it. The copy never
 * finished and the boot log stopped mid-sentence, every time.
 *
 * So core 0 opens the link when it is done with the flash, immediately before
 * handing over. Nothing is lost: the console up to that point goes out over USB,
 * which is the independent channel that does not depend on the link working.
 */
static volatile bool blk_link_enabled;

void frank_link_enable(void)
{
    frank_ring_barrier();
    blk_link_enabled = true;
}

/* Diagnostics for the one transaction that happens before Linux exists. Core 1
 * has no other way to say what went wrong, and "capacity 0" on the kernel side
 * is the same answer for every possible failure. */
static size_t blk_rx_remaining;
static int32_t blk_init_status = -19;
static bool blk_probe_done;

void frank_ring_puts(const char *s);
uint32_t frank_console_take(uint8_t *dst, uint32_t max);
void frank_console_feed(const uint8_t *src, uint32_t n);

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
    /*
     * Bytes actually put on the wire.
     *
     * link_tx_start() sends `bytes / 4` words and drops any remainder without
     * saying so -- the PIO FIFO is 32 bits wide and every earlier caller used
     * sector- and header-sized frames, so nothing ever noticed. The console is
     * the first caller with an arbitrary length, and an unpadded 37-byte write
     * put 36 bytes on the wire while the receiver waited for 37. The leftover
     * shifted every following transfer, which showed up as a boot log that
     * started clean and decayed into fragments of itself.
     *
     * So console frames are padded up to a word. The true length travels in the
     * request header, and the master uses that rather than what it received.
     */
    uint32_t bytes = (op == LINK_BLK_OP_CON) ? ((count + 3u) & ~3u)
                                             : count * LINK_BLK_SECTOR;

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
    link_blk_rsp_t rsp = { .status = -5, .value = 0 };
    uint32_t reply_bytes = sizeof(rsp);

    if (op == LINK_BLK_OP_READ)
        reply_bytes += bytes;
    else if (op == LINK_BLK_OP_CON)
        reply_bytes += LINK_CON_MAX_KEYS;

    if (!blk_link_up)
        return -19;                                     /* -ENODEV */

    /*
     * How long to wait for the master to answer the doorbell.
     *
     * A block request can legitimately take a while -- the master may be part
     * way through a card access -- so those wait seconds. Console and capacity
     * requests must not: they run on the same core that services USB, and a
     * five-second wait for a master that simply is not up yet freezes the
     * console mid-line. That is exactly what it did. The master needs 4.5 s to
     * initialise its display, keyboard and card; the slave asked at 1 s, blocked
     * for five, and took the boot log with it.
     *
     * When the master is there it answers in about 141 us (F3), so 20 ms is
     * generous and a missing master now costs 20 ms instead of five seconds.
     */
    uint32_t db_timeout = (op == LINK_BLK_OP_READ || op == LINK_BLK_OP_WRITE)
                          ? 5000000u : 20000u;

    /* Raise our doorbell and wait for the master to acknowledge with its own.
     * The master polls DB_SM for exactly this. */
    link_db_set(&blk_link, true);
    if (!link_db_wait(&blk_link, true, db_timeout)) {
        link_db_set(&blk_link, false);
        return -110;                                    /* -ETIMEDOUT */
    }

    link_use_ctrl_rate(&blk_link);

    /* Armed first, before a single byte of the request goes out. */
    link_rx_arm(&blk_link, reply, reply_bytes);

    link_tx_start(&blk_link, &req, sizeof(req));
    if (!link_tx_finish(&blk_link, 1000000))
        goto fail;

    if (op == LINK_BLK_OP_WRITE || (op == LINK_BLK_OP_CON && bytes)) {
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
    else if (rsp.status == 0 && op == LINK_BLK_OP_CON && rsp.value) {
        uint32_t keys = rsp.value > LINK_CON_MAX_KEYS ? LINK_CON_MAX_KEYS
                                                      : rsp.value;
        frank_console_feed(reply + sizeof(rsp), keys);
    }

    if (capacity_out)
        *capacity_out = rsp.value;

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

/*
 * Ask the master how big the device is. Retried until it answers.
 *
 * A single attempt was wrong: the two halves are reset independently and the
 * master takes about 4.5 s to bring up its display, keyboard and card, so a
 * probe at one second finds nobody home. One shot meant that ordering decided
 * whether the machine had a disk and a console at all, and it failed silently --
 * capacity zero looks identical to no card.
 *
 * Retrying costs 20 ms per attempt while the master is absent and stops as soon
 * as it answers, so neither half has to be started first.
 */
static void blk_probe(void)
{
    frank_ring_shared_t *s = FRANK_RING_SHARED;
    uint32_t capacity = 0;

    blk_init_status = blk_transact(LINK_BLK_OP_INFO, 0, 0, NULL, &capacity);
    if (blk_init_status != 0)
        return;

    blk_report("blk: capacity=", (int32_t)capacity);

    frank_ring_barrier();
    s->blk.capacity = capacity;
    blk_probe_done = true;
}

/*
 * Is the master's terminal there to receive console output?
 *
 * Answering "no" until the master has proved it is listening is what stops the
 * console fan-out from holding bytes for a consumer that does not exist. The
 * capacity probe is that proof: it is a round trip the master had to answer.
 */
bool frank_link_console_up(void)
{
    return blk_probe_done && blk_init_status == 0;
}

/*
 * One console transaction: hand over whatever Linux has written, take back
 * whatever was typed.
 *
 * Rate-limited rather than run every pass. The link costs 141 us per control
 * round trip idle and 2753 us with the master's USB HID host running (F3), and
 * a service loop that spins at tens of kHz would spend the entire link on
 * asking "anything typed?". 200 Hz is far faster than a person types and leaves
 * the wire almost entirely free for block traffic. Output does not wait for the
 * timer: if there is anything to show, it goes immediately.
 */
static void console_pump(void)
{
    static uint8_t out[LINK_CON_MAX_TX + 4] __attribute__((aligned(4)));
    static uint32_t next_poll_us;
    uint32_t now = time_us_32();
    uint32_t n = frank_console_take(out, LINK_CON_MAX_TX);

    if (!n && (int32_t)(now - next_poll_us) < 0)
        return;
    next_poll_us = now + 5000u;

    /* Clear the pad so stale bytes from an earlier, longer frame are not what
     * gets transmitted. The master ignores them, but only because it is told
     * the real length; leaving them undefined makes a protocol bug invisible. */
    for (uint32_t i = n; i < ((n + 3u) & ~3u); i++)
        out[i] = 0;

    blk_transact(LINK_BLK_OP_CON, 0, n, out, NULL);
}

/*
 * Called from core 1's service loop. Polls rather than takes an interrupt:
 * there is no cross-core doorbell wired for this, and core 1 has nothing else
 * to do between USB passes.
 */
void frank_blk_service(void)
{
    frank_ring_shared_t *s = FRANK_RING_SHARED;
    static uint32_t warmup;
    static uint32_t next_probe_us;
    uint32_t seq;

    /* Not until core 0 is finished with the flash and has handed over. */
    if (!blk_link_enabled)
        return;

    /* Let USB enumerate before touching the link at all. */
    if (++warmup < 200000u)
        return;

    /* Keep asking until the master answers, then never again. */
    if (!blk_probe_done) {
        uint32_t now = time_us_32();
        if ((int32_t)(now - next_probe_us) < 0)
            return;
        next_probe_us = now + 250000u;
        blk_probe();
        return;
    }

    console_pump();

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
