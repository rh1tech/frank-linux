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
 * This stage serves storage only, and from a RAM disk rather than the card.
 * Proving the block path with memory on this end means that when the SD driver
 * goes in, a failure is the card and not the protocol -- the same reason the
 * console was proven with core 0 standing in for Linux before Linux existed.
 *
 * Deliberately does NOT bring up HDMI or the USB HID host yet: F3 measured that
 * enabling the HID host costs this half 3x its memory bandwidth and 20x its link
 * round-trip, and mixing that into the first block bring-up would confuse
 * "slow" with "broken".
 */

#include <stdio.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/structs/qmi.h"
#include "hardware/vreg.h"
#include "pico/stdlib.h"

#include "link_blk.h"
#include "link_bus.h"

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

/*
 * RAM disk. 64 kB is enough to carry a filesystem superblock and prove reads,
 * writes and readback without touching the card; the master's own 8 MB of PSRAM
 * is free (Linux runs on the other half) if this needs to grow.
 */
#define RAMDISK_SECTORS  128u
static uint8_t ramdisk[RAMDISK_SECTORS * LINK_BLK_SECTOR];

static uint8_t xfer[LINK_BLK_MAX_SECTORS * LINK_BLK_SECTOR] __attribute__((aligned(4)));

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

/* Fill the RAM disk with something a read can be checked against without the
 * two sides exchanging reference data: sector N starts with N. */
static void ramdisk_init(void)
{
    for (uint32_t s = 0; s < RAMDISK_SECTORS; s++) {
        uint8_t *p = ramdisk + s * LINK_BLK_SECTOR;
        for (uint32_t i = 0; i < LINK_BLK_SECTOR; i += 4) {
            uint32_t v = (s << 16) | i;
            p[i] = (uint8_t)v; p[i+1] = (uint8_t)(v >> 8);
            p[i+2] = (uint8_t)(v >> 16); p[i+3] = (uint8_t)(v >> 24);
        }
    }
}

static int32_t do_request(const link_blk_req_t *req)
{
    if (req->magic != LINK_BLK_MAGIC)
        return -22;                                     /* -EINVAL */
    if (req->op == LINK_BLK_OP_INFO)
        return 0;
    if (req->count == 0 || req->count > LINK_BLK_MAX_SECTORS)
        return -22;
    if (req->lba + req->count > RAMDISK_SECTORS)
        return -5;                                      /* -EIO */
    return 0;
}

int main(void)
{
    clocks_bringup();
    stdio_init_all();

    for (int i = 0; i < 6; i++) {
        printf("master-ioserver - waiting for console (%d/6)\n", i + 1);
        sleep_ms(200);
    }
    printf("\n=== FRANK master I/O server ===\n");
    printf("sys_clk %u MHz, ramdisk %u sectors\n",
           (unsigned)(clock_get_hz(clk_sys) / 1000000u), (unsigned)RAMDISK_SECTORS);

    ramdisk_init();

    link_t link;
    link_init(&link, pio0, LINK_TX_BASE, LINK_RX_BASE,
              LINK_DB_OUT, LINK_DB_IN, LINK_FS, true);
    link_use_ctrl_rate(&link);
    printf("RESULT ioserver ready\n");

    uint32_t served = 0, errors = 0;

    for (;;) {
        /*
         * Poll the slave's doorbell. This is the direction the bring-up
         * firmware never used: there, the master always went first. Storage is
         * driven by Linux, so the slave has to be able to start a transaction.
         */
        if (!link_db_get(&link)) {
            tight_loop_contents();
            continue;
        }

        link_blk_req_t req;
        link_rx_arm(&link, &req, sizeof(req));
        link_db_set(&link, true);                       /* "ready" */

        if (link_rx_wait(&link, 1000000) != 0) {
            errors++;
            link_db_set(&link, false);
            while (link_db_get(&link)) tight_loop_contents();
            continue;
        }

        int32_t status = do_request(&req);
        uint32_t bytes = (status == 0 && req.op != LINK_BLK_OP_INFO)
                         ? req.count * LINK_BLK_SECTOR : 0;

        if (status == 0 && req.op == LINK_BLK_OP_WRITE) {
            link_rx_arm(&link, xfer, bytes);
            if (link_rx_wait(&link, 2000000) != 0)
                status = -5;
            else
                memcpy(ramdisk + req.lba * LINK_BLK_SECTOR, xfer, bytes);
        }

        link_blk_rsp_t rsp = { .status = status, .capacity = RAMDISK_SECTORS };
        link_tx_start(&link, &rsp, sizeof(rsp));
        link_tx_finish(&link, 1000000);

        if (status == 0 && req.op == LINK_BLK_OP_READ) {
            memcpy(xfer, ramdisk + req.lba * LINK_BLK_SECTOR, bytes);
            link_tx_start(&link, xfer, bytes);
            if (!link_tx_finish(&link, 2000000))
                errors++;
        }

        link_db_set(&link, false);
        while (link_db_get(&link))
            tight_loop_contents();

        if (status == 0) served++; else errors++;
        if ((served + errors) % 32u == 0)
            printf("RESULT ioserver served=%u errors=%u\n",
                   (unsigned)served, (unsigned)errors);
    }
}
