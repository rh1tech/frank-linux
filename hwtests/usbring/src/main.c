/*
 * Phase 4 de-risk: prove the core0 <-> core1 <-> USB path before Linux exists.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Core 1 runs the TinyUSB CDC service; core 0 stands in for Linux, writing to
 * the TX ring and echoing whatever arrives on RX. If this works, the Linux
 * driver is a ring-buffer tty and nothing more -- which is the whole reason for
 * the design, since Linux cannot drive the RP2350's USB controller itself.
 *
 * Core 0 also reports through UART0 so the two channels can be compared: the
 * UART is the independent one that keeps working when our own code does not.
 */

#include <stdio.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/structs/qmi.h"
#include "hardware/vreg.h"
#include "pico/stdlib.h"

#include "frank_ring.h"

#ifndef CPU_SPEED
#define CPU_SPEED 252
#endif
#ifndef FLASH_MAX_FREQ_MHZ
#define FLASH_MAX_FREQ_MHZ 66
#endif

void frank_core1_usb_start(void);

/* Same sequence afboot-rp2350 will need: the divider must suit the clock we are
 * about to run at, programmed from RAM before the clock rises (hw-findings F10). */
static uint32_t __no_inline_not_in_flash_func(flash_set_clkdiv)(uint32_t clock_hz)
{
    const uint32_t max_hz = FLASH_MAX_FREQ_MHZ * 1000000u;
    uint32_t divisor = (clock_hz + max_hz - 1u) / max_hz;
    if (divisor < 1u) divisor = 1u;
    if (divisor > 255u) divisor = 255u;

    uint32_t rxdelay = divisor;
    if (clock_hz / divisor > 100000000u)
        rxdelay += 1u;

    uint32_t timing = qmi_hw->m[0].timing;
    timing &= ~(QMI_M0_TIMING_CLKDIV_BITS | QMI_M0_TIMING_RXDELAY_BITS);
    timing |= (divisor << QMI_M0_TIMING_CLKDIV_LSB)
            | (rxdelay << QMI_M0_TIMING_RXDELAY_LSB);
    qmi_hw->m[0].timing = timing;
    __asm__ volatile("dmb" ::: "memory");
    for (volatile int i = 0; i < 1000; i++)
        __asm__ volatile("nop");
    return divisor;
}

int main(void)
{
#if CPU_SPEED >= 300
    vreg_disable_voltage_limit();
#endif
#if CPU_SPEED >= 504
    vreg_set_voltage(VREG_VOLTAGE_1_65);
#elif CPU_SPEED >= 300
    vreg_set_voltage(VREG_VOLTAGE_1_60);
#else
    vreg_set_voltage(VREG_VOLTAGE_1_50);
#endif
    sleep_ms(10);
    flash_set_clkdiv(CPU_SPEED * 1000000u);
    set_sys_clock_khz(CPU_SPEED * 1000, true);

    /* Console independent of the overclock and of clk_sys (hw-findings F10). */
    clock_configure(clk_peri, 0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                    48 * MHZ, 48 * MHZ);
    stdio_init_all();

    for (int i = 0; i < 6; i++) {
        printf("hwtest usbring - waiting for console (%d/6)\n", i + 1);
        sleep_ms(200);
    }

    printf("\n=== core0/core1 USB CDC ring ===\n");
    printf("sys_clk %u MHz\n", (unsigned)(clock_get_hz(clk_sys) / 1000000u));

    frank_core1_usb_start();

    frank_ring_shared_t *s = FRANK_RING_SHARED;
    printf("RESULT ring base=0x%08x magic=0x%08x version=%u\n",
           (unsigned)FRANK_RING_BASE, (unsigned)s->magic, (unsigned)s->version);

    /* Give core 1 a moment, then check it is actually turning over rather than
     * merely having started. */
    uint32_t a = s->core1_alive;
    sleep_ms(200);
    uint32_t b = s->core1_alive;
    printf("RESULT core1 alive_delta=%u running=%u\n",
           (unsigned)(b - a), (unsigned)(b != a));

    /* Stand in for the kernel: announce ourselves on the USB console, then echo
     * whatever the host types, so the path can be exercised in both directions
     * from a terminal. */
    static const char banner[] =
        "\r\nfrank-linux core1 USB CDC ring service\r\n"
        "core0 is standing in for Linux; type and it will echo.\r\n";
    frank_ring_put(&s->tx, (const uint8_t *)banner, sizeof(banner) - 1);

    uint32_t echoed = 0, ticks = 0;
    for (;;) {
        uint8_t buf[64];
        uint32_t n = frank_ring_get(&s->rx, buf, sizeof(buf));
        if (n) {
            frank_ring_put(&s->tx, buf, n);
            echoed += n;
        }
        if (++ticks % 2000000u == 0)
            printf("RESULT echo bytes=%u core1_alive=%u\n",
                   (unsigned)echoed, (unsigned)s->core1_alive);
        if (ticks == 2000000u)
            printf("=== done ===\n");
    }
}
