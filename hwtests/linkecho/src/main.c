/*
 * Phase 5 step 1: prove the inter-chip link under our own firmware.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The FRANK bring-up firmware already showed the wire is good -- 96.1 MiB/s,
 * zero byte errors (hw-findings F2). What is unproven is the link running under
 * *our* code, on the half that also runs Linux, with core 1 serving USB at the
 * same time.
 *
 * Master sends a counting pattern; the slave checks it and reports over the USB
 * console that core 1 already provides. If this works, a block device over the
 * link is plumbing rather than research.
 *
 * Built for both halves from one source: the only differences are which pins
 * each end drives and where its console goes.
 */

#include <stdio.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/structs/qmi.h"
#include "hardware/vreg.h"
#include "pico/stdlib.h"

#include "link_bus.h"

#ifndef CPU_SPEED
#define CPU_SPEED 252
#endif
#ifndef FLASH_MAX_FREQ_MHZ
#define FLASH_MAX_FREQ_MHZ 66
#endif

/*
 * Pin map, from frank_core2_proto_board.h. Both buses use the same relative
 * layout -- clock at data_base + 8, valid at data_base + 9 -- which is what
 * lets one pair of PIO programs serve either direction on either chip.
 *
 *   bus A  master GPIO20..29 -> slave GPIO1..10    (master to slave)
 *   bus B  slave  GPIO11..20 -> master GPIO30..39  (slave to master)
 */
#ifdef FRANK_IS_MASTER
#define LINK_TX_BASE   20
#define LINK_RX_BASE   30
#define LINK_DB_OUT    41      /* DB_MS, master drives */
#define LINK_DB_IN     42      /* DB_SM, slave drives  */
#define LINK_FS        40
#define LINK_FS_OUT    true
#else
#define LINK_TX_BASE   11
#define LINK_RX_BASE   1
#define LINK_DB_OUT    23      /* DB_SM */
#define LINK_DB_IN     22      /* DB_MS */
#define LINK_FS        21
#define LINK_FS_OUT    false
#endif

#define BLOCK_BYTES    1024u
#define BLOCKS         64u

void frank_core1_usb_start(void);
bool frank_console_ready(void);

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

/* Same LFSR the FRANK firmware uses, so both ends can generate the expected
 * bytes from a seed and neither has to send reference data across the link it
 * is trying to test. */
static void fill_pattern(uint8_t *buf, uint32_t bytes, uint32_t seed)
{
    uint32_t lfsr = seed ? seed : 0xACE1u;
    for (uint32_t i = 0; i < bytes; i++) {
        lfsr ^= lfsr << 13;
        lfsr ^= lfsr >> 17;
        lfsr ^= lfsr << 5;
        buf[i] = (uint8_t)lfsr;
    }
}

static uint8_t txbuf[BLOCK_BYTES] __attribute__((aligned(4)));
static uint8_t rxbuf[BLOCK_BYTES] __attribute__((aligned(4)));

int main(void)
{
    clocks_bringup();

#ifdef FRANK_IS_MASTER
    stdio_init_all();
    for (int i = 0; i < 6; i++) {
        printf("hwtest linkecho (master) - waiting for console (%d/6)\n", i + 1);
        sleep_ms(200);
    }
    printf("\n=== link echo, master ===\n");
#else
    /* The slave reports through the console core 1 already provides; there is
     * no wired UART on this half. */
    frank_core1_usb_start();
    for (int i = 0; i < 1000 && !frank_console_ready(); i++)
        sleep_ms(10);
    extern void frank_ring_puts(const char *s);
#endif

    link_t link;
    link_init(&link, pio0, LINK_TX_BASE, LINK_RX_BASE,
              LINK_DB_OUT, LINK_DB_IN, LINK_FS, LINK_FS_OUT);

#ifdef FRANK_IS_MASTER
    printf("RESULT link init tx_base=%u rx_base=%u\n",
           (unsigned)LINK_TX_BASE, (unsigned)LINK_RX_BASE);

    uint32_t sent = 0, timeouts = 0;
    for (uint32_t b = 0; b < BLOCKS; b++) {
        fill_pattern(txbuf, BLOCK_BYTES, 0x1234u + b);

        /* Doorbell up, stream, doorbell down -- the handshake means neither
         * side has to agree about absolute time. */
        link_db_set(&link, true);
        if (!link_db_wait(&link, true, 500000)) { timeouts++; link_db_set(&link, false); continue; }

        link_use_ctrl_rate(&link);
        link_tx_start(&link, txbuf, BLOCK_BYTES);
        if (!link_tx_finish(&link, 500000)) timeouts++;
        else sent += BLOCK_BYTES;

        link_db_set(&link, false);
        link_db_wait(&link, false, 500000);
    }
    printf("RESULT link master sent=%u timeouts=%u\n",
           (unsigned)sent, (unsigned)timeouts);
    printf("=== done ===\n");
#else
    uint32_t got = 0, bad = 0, blocks = 0, timeouts = 0;
    for (uint32_t b = 0; b < BLOCKS; b++) {
        if (!link_db_wait(&link, true, 5000000)) { timeouts++; break; }

        link_rx_arm(&link, rxbuf, BLOCK_BYTES);
        link_db_set(&link, true);

        if (link_rx_wait(&link, 500000) != 0) timeouts++;
        else {
            fill_pattern(txbuf, BLOCK_BYTES, 0x1234u + b);
            for (uint32_t i = 0; i < BLOCK_BYTES; i++)
                if (rxbuf[i] != txbuf[i]) bad++;
            got += BLOCK_BYTES;
            blocks++;
        }

        link_db_wait(&link, false, 500000);
        link_db_set(&link, false);
    }

    char msg[128];
    snprintf(msg, sizeof(msg),
             "\r\nRESULT link slave blocks=%u bytes=%u bad=%u timeouts=%u\r\n"
             "=== done ===\r\n",
             (unsigned)blocks, (unsigned)got, (unsigned)bad, (unsigned)timeouts);
    frank_ring_puts(msg);
#endif

    while (true)
        tight_loop_contents();
}
