/*
 * FRANK Core 2 Proto master: I/O server for the half that runs Linux.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The slave runs Linux and has almost nothing attached to it. Everything
 * user-facing is on this half -- microSD (J7), HDMI (J5), the USB HID keyboard
 * (J8), the I2S DAC -- so this firmware answers for all of it over the link.
 *
 * This stage serves storage only: the microSD on SPI0 (J7), or a RAM disk if no
 * card is present. Proving the block path against memory first meant that when
 * the card went in, a failure was the card and not the protocol -- the same
 * reason the console was proven with core 0 standing in for Linux.
 *
 * It also drives the HDMI console: DispHSTX scans out a 640x480p60 DVI signal
 * and the VersaTerm-derived terminal engine turns the byte stream from Linux
 * into 80x25 text. Both are Protea's, unmodified -- the FRANK master's HDMI pin
 * map is byte-identical to Protea's DISPHSTX_DVI_PINOUT 2, so the video path
 * needed no porting at all. What changed is only where the terminal's bytes
 * come from and go to (term/term_compat.c).
 *
 * The USB keyboard is on J8. Keystrokes go the other way down the same link:
 * terminal engine -> term_compat's queue -> the next console transaction ->
 * the slave's ring -> ttyFRK0 -> the shell. Linux sees an ordinary tty.
 *
 * F3 measured that running the HID host costs this half 3x its memory bandwidth
 * and 20x its link round-trip, which is why it went in last, after the display
 * and the disk were both known good.
 */

#include <stdio.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/structs/qmi.h"
#include "hardware/vreg.h"
#include "pico/stdlib.h"

#include "link_blk.h"
#include "link_bus.h"
#include "display.h"
#include "framebuf.h"
#include "storage.h"
#include "input.h"
#include "keyq.h"
#include "term_input.h"
#include "terminal.h"
#include "usbhid.h"

#ifndef CPU_SPEED
#define CPU_SPEED 252
#endif
#ifndef FLASH_MAX_FREQ_MHZ
#define FLASH_MAX_FREQ_MHZ 66
#endif

/* Link pins, master end. From frank_core2_proto_board.h. */
#define LINK_TX_BASE   20      /* bus A, master -> slave */
#define LINK_RX_BASE   30      /* bus B, slave -> master */
#define LINK_DB_OUT    41      /* DB_MS */
#define LINK_DB_IN     42      /* DB_SM */
#define LINK_FS        40

static uint32_t capacity_sectors;
static const char *medium = "none";

static uint8_t xfer[LINK_BLK_MAX_SECTORS * LINK_BLK_SECTOR] __attribute__((aligned(4)));
static uint8_t reply[sizeof(link_blk_rsp_t)
                     + LINK_BLK_MAX_SECTORS * LINK_BLK_SECTOR]
              __attribute__((aligned(4)));

static uint32_t __no_inline_not_in_flash_func(flash_set_clkdiv)(uint32_t clock_hz)
{
    const uint32_t max_hz = FLASH_MAX_FREQ_MHZ * 1000000u;
    uint32_t divisor = (clock_hz + max_hz - 1u) / max_hz;
    if (divisor < 1u) divisor = 1u;
    if (divisor > 255u) divisor = 255u;
    uint32_t rxdelay = divisor;
    if (clock_hz / divisor > 100000000u) rxdelay += 1u;

    uint32_t t = qmi_hw->m[0].timing;
    t &= ~(QMI_M0_TIMING_CLKDIV_BITS | QMI_M0_TIMING_RXDELAY_BITS);
    t |= (divisor << QMI_M0_TIMING_CLKDIV_LSB)
       | (rxdelay << QMI_M0_TIMING_RXDELAY_LSB);
    qmi_hw->m[0].timing = t;
    __asm__ volatile("dmb" ::: "memory");
    for (volatile int i = 0; i < 1000; i++) __asm__ volatile("nop");
    return divisor;
}

static void clocks_bringup(void)
{
#if CPU_SPEED >= 300
    vreg_disable_voltage_limit();
    vreg_set_voltage(VREG_VOLTAGE_1_60);
#else
    vreg_set_voltage(VREG_VOLTAGE_1_50);
#endif
    sleep_ms(10);
    flash_set_clkdiv(CPU_SPEED * 1000000u);
    set_sys_clock_khz(CPU_SPEED * 1000, true);
    clock_configure(clk_peri, 0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                    48 * MHZ, 48 * MHZ);
}

static int32_t do_request(const link_blk_req_t *req)
{
    if (req->magic != LINK_BLK_MAGIC)
        return -22;                                     /* -EINVAL */
    if (req->op == LINK_BLK_OP_INFO)
        return 0;
    if (req->op == LINK_BLK_OP_CON)
        return req->count <= LINK_CON_MAX_TX ? 0 : -22;
    if (req->count == 0 || req->count > LINK_BLK_MAX_SECTORS)
        return -22;
    if (req->lba + req->count > capacity_sectors)
        return -5;                                      /* -EIO */
    return 0;
}

/*
 * Keyboard, from either of two places.
 *
 * The USB HID keyboard on J8 is the real one. The master's own UART console is
 * the second, and it is not a debug afterthought: it is what lets the test
 * harness type into the machine and read the answer off the HDMI capture,
 * exercising the identical path a keystroke takes -- through the terminal
 * engine, the link, and the tty -- without a finger on a key. The alternative
 * was a separate self-test build, which would have proven a binary nobody ships.
 */
static void keyboard_tick(void)
{
#if FRANK_MASTER_HID
    usbhid_task();
#endif

    input_event_t ev;
    while (input_poll(&ev))
        terminal_feed_event(&ev);

    int c = getchar_timeout_us(0);
    if (c != PICO_ERROR_TIMEOUT && c >= 0) {
        input_event_t k = { .code = (c == '\r' || c == '\n') ? KEY_ENTER : c };
        terminal_feed_event(&k);
    }
}

/*
 * Wait for the slave to drop its doorbell, but not forever.
 *
 * This used to be `while (link_db_get(&link));`, which is correct only while
 * the slave is alive. Reset the slave mid-transaction and DB_SM goes high
 * impedance -- the master reads it as still raised and spins there for good.
 * The symptom was a machine that worked exactly once: the first Linux boot was
 * fine, and every reset after it found a master that had stopped answering,
 * with no message to say why because it was still, technically, running.
 *
 * The other half of the fix is that the display and keyboard keep being
 * serviced while waiting, so a slave that goes away mid-transfer cannot freeze
 * the screen either.
 */
static bool wait_db_clear(link_t *link, uint32_t timeout_us)
{
    absolute_time_t deadline = make_timeout_time_us(timeout_us);

    while (link_db_get(link)) {
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0)
            return false;
        display_tick();
        keyboard_tick();
    }
    return true;
}

int main(void)
{
    clocks_bringup();
    stdio_init_all();

    /*
     * Video first, because DispHstxSelDispMode() repoints clk_peri at clk_sys.
     * Any UART divisor computed before that comes out 252/48 = 5.25x too fast,
     * which is Protea's documented trap. Re-pin clk_peri to the USB PLL after,
     * then re-init stdio so the console baud is exact again.
     */
    display_init();
    clock_configure(clk_peri, 0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                    48 * MHZ, 48 * MHZ);
    stdio_init_all();

    framebuf_init();
    terminal_init();
    input_init();
#if FRANK_MASTER_HID
    usbhid_init();
#endif

    for (int i = 0; i < 6; i++) {
        printf("master-ioserver - waiting for console (%d/6)\n", i + 1);
        sleep_ms(200);
    }
    printf("\n=== FRANK master I/O server ===\n");
    capacity_sectors = storage_init(&medium);

    printf("sys_clk %u MHz, medium %s, %u sectors (%u KiB)\n",
           (unsigned)(clock_get_hz(clk_sys) / 1000000u), medium,
           (unsigned)capacity_sectors, (unsigned)(capacity_sectors / 2u));

    link_t link;
    link_init(&link, pio0, LINK_TX_BASE, LINK_RX_BASE,
              LINK_DB_OUT, LINK_DB_IN, LINK_FS, true);
    link_use_ctrl_rate(&link);
    printf("RESULT ioserver ready\n");

    /* Something for the capture card to decode before Linux says anything, so
     * a blank screen can be told apart from a screen nobody has written to. */
    terminal_receive_string("\033[2J\033[HFRANK master I/O server ready\r\n");
    terminal_receive_string("HDMI 640x480p60, 80x25, medium ");
    terminal_receive_string(medium);
    terminal_receive_string("\r\n");

    uint32_t served = 0, errors = 0;

    for (;;) {
        /*
         * Poll the slave's doorbell. This is the direction the bring-up
         * firmware never used: there, the master always went first. Storage is
         * driven by Linux, so the slave has to be able to start a transaction.
         */
        /* Heartbeat while idle, so "waiting for a doorbell that never comes"
         * is distinguishable from "wedged" without a debugger. */
        if (!link_db_get(&link)) {
            static uint32_t spins;
            display_tick();
            keyboard_tick();
            if (++spins % 20000000u == 0)
                printf("RESULT ioserver idle db_in=%u served=%u errors=%u\n",
                       (unsigned)link_db_get(&link),
                       (unsigned)served, (unsigned)errors);
            continue;
        }
        if (0)
            printf("RESULT ioserver doorbell seen\n");

        link_blk_req_t req;
        link_rx_arm(&link, &req, sizeof(req));
        link_db_set(&link, true);                       /* "ready" */

        if (link_rx_wait(&link, 1000000) != 0) {
            errors++;
            link_db_set(&link, false);
            wait_db_clear(&link, 1000000);
            continue;
        }

        int32_t status = do_request(&req);
        uint32_t bytes = 0;
        if (status == 0 && req.op == LINK_BLK_OP_CON)
            bytes = req.count;                       /* console: raw bytes */
        else if (status == 0 && req.op != LINK_BLK_OP_INFO)
            bytes = req.count * LINK_BLK_SECTOR;

        /*
         * Console output from Linux, straight into the terminal engine.
         *
         * The frame on the wire is padded up to a whole word because the link's
         * DMA moves 32-bit words and silently drops any remainder. The header
         * carries the real length, so receive the padded size and use only what
         * was actually sent.
         */
        if (status == 0 && req.op == LINK_BLK_OP_CON && bytes) {
            uint32_t padded = (bytes + 3u) & ~3u;

            link_rx_arm(&link, xfer, padded);
            if (link_rx_wait(&link, 1000000) != 0) {
                status = -5;
            } else {
                for (uint32_t i = 0; i < bytes; i++)
                    terminal_receive_char(xfer[i]);
            }
        }

        if (status == 0 && req.op == LINK_BLK_OP_WRITE) {
            link_rx_arm(&link, xfer, bytes);
            if (link_rx_wait(&link, 2000000) != 0)
                status = -5;
            else
                status = storage_write(req.lba, req.count, xfer);
        }

        /* Status and any read data as one transfer, so the slave arms once and
         * there is no window between two sends for it to miss. */
        if (status == 0 && req.op == LINK_BLK_OP_READ)
            status = storage_read(req.lba, req.count, reply + sizeof(link_blk_rsp_t));

        link_blk_rsp_t rsp = { .status = status, .value = capacity_sectors };
        uint32_t reply_bytes = sizeof(rsp);

        if (status == 0 && req.op == LINK_BLK_OP_READ) {
            reply_bytes += bytes;
        } else if (req.op == LINK_BLK_OP_CON) {
            /* Always the full key buffer, valid count in the header: the slave
             * has to arm for an exact size before it knows what is waiting. */
            memset(reply + sizeof(rsp), 0, LINK_CON_MAX_KEYS);
            rsp.value = term_compat_take_keys(reply + sizeof(rsp),
                                              LINK_CON_MAX_KEYS);
            reply_bytes += LINK_CON_MAX_KEYS;
        }

        memcpy(reply, &rsp, sizeof(rsp));
        link_tx_start(&link, reply, reply_bytes);
        if (!link_tx_finish(&link, 2000000))
            errors++;

        link_db_set(&link, false);
        if (!wait_db_clear(&link, 1000000)) {
            /* The slave stopped talking part way through -- almost always a
             * reset on that side. Abandon this transaction and go back to
             * polling; the next boot must find a master that still answers. */
            link_rx_abort(&link);
            errors++;
        }

        if (status == 0) served++; else errors++;
        /* Console transactions run at 200 Hz; logging each one would drown the
         * console in its own bookkeeping. */
        if (req.op != LINK_BLK_OP_CON)
            printf("RESULT ioserver op=%u served=%u errors=%u\n",
                   (unsigned)req.op, (unsigned)served, (unsigned)errors);
    }
}
