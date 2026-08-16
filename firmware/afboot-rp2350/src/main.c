/*
 * afboot-rp2350 - bring up the slave half and hand over to Linux.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Named after afboot-stm32, which does the same job for the STM32F4 NOMMU
 * targets in Buildroot: the smallest thing that can put a Cortex-M into a state
 * where a Linux kernel can start.
 *
 * Order matters here, and most of it was learned the hard way; see
 * docs/hw-findings.md F10.
 *
 *   1. regulator      -- above 1.30 V needs the limit disabled first, or the
 *                        request is silently clamped and the chip browns out
 *   2. flash divider  -- for the clock we are ABOUT to use, programmed from
 *                        RAM, before raising it. set_sys_clock_khz() returns
 *                        into flash-resident code, so raising first means the
 *                        next instruction fetch happens at the new speed with
 *                        boot2's old divider
 *   3. system clock
 *   4. clk_peri       -- pinned to the USB PLL so peripherals do not follow
 *                        the system clock
 *   5. PSRAM          -- QMI CS1, which becomes Linux's system RAM. Must come
 *                        BEFORE core 1 is launched: psram_init() takes the QMI
 *                        out of XIP to configure it, and core 1 executing
 *                        TinyUSB from flash dies mid-fetch when that happens.
 *                        The FRANK firmware documents the same constraint for
 *                        its flash probe versus graphics_init().
 *   6. core 1         -- the USB CDC console service, started before Linux so
 *                        the kernel's first printk has somewhere to go
 *   7. copy + jump    -- DTB and kernel into PSRAM, VTOR, then the ARM boot
 *                        protocol
 */

#include <string.h>

#include "hardware/clocks.h"
#include "hardware/structs/qmi.h"
#include "hardware/vreg.h"
#include "pico/bootrom.h"
#include "pico/stdlib.h"

#include "frank_ring.h"
#include "psram_init.h"

#ifndef CPU_SPEED
#define CPU_SPEED 252
#endif
#ifndef FLASH_MAX_FREQ_MHZ
#define FLASH_MAX_FREQ_MHZ 66
#endif

/* ---- flash layout, agreed with tools/flash-slave.sh ------------------- */

#define FLASH_BASE          0x10000000u
/* The bootloader itself occupies the first 512 kB; payloads live above it so a
 * kernel rebuild never has to touch the bootloader's own image. */
#define FLASH_DTB_ADDR      (FLASH_BASE + 0x000F0000u)
#define FLASH_KERNEL_ADDR   (FLASH_BASE + 0x00100000u)

/* ---- PSRAM layout, agreed with the DTS -------------------------------- */

#define PSRAM_BASE          0x11000000u
#define PSRAM_NOCACHE_BASE  0x15000000u
/* DTB in the 32 kB below the kernel, exactly as the QEMU target does it, so the
 * two layouts stay comparable. */
#define RAM_DTB_ADDR        (PSRAM_BASE + 0x00000000u)
#define RAM_KERNEL_ADDR     (PSRAM_BASE + 0x00008000u)

/* An FDT begins with this, big-endian. Used to tell a real payload from erased
 * flash before copying megabytes of 0xFF into PSRAM and jumping into it. */
#define FDT_MAGIC           0xD00DFEEDu

/* A payload with no size field would mean copying the whole flash window. Both
 * images are preceded by a little-endian length word written by the flash
 * script, so the bootloader copies exactly what is there. */
typedef struct {
    uint32_t magic;         /* 'FRPL' */
    uint32_t length;        /* bytes following this header */
} payload_hdr_t;

#define PAYLOAD_MAGIC       0x4c505246u

void frank_core1_usb_start(void);
bool frank_console_ready(void);

/* ---- clocks ----------------------------------------------------------- */

static uint32_t __no_inline_not_in_flash_func(flash_set_clkdiv)(uint32_t clock_hz)
{
    const uint32_t max_hz = FLASH_MAX_FREQ_MHZ * 1000000u;
    uint32_t divisor = (clock_hz + max_hz - 1u) / max_hz;
    if (divisor < 1u)
        divisor = 1u;
    if (divisor > 255u)
        divisor = 255u;

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

static void clocks_bringup(void)
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

    clock_configure(clk_peri, 0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                    48 * MHZ, 48 * MHZ);
}

/* ---- PSRAM ------------------------------------------------------------ */

static size_t psram_probe_bytes(void)
{
    volatile uint32_t *base = (volatile uint32_t *)PSRAM_NOCACHE_BASE;
    const uint32_t marker = 0x5AA51234u;

    base[0] = marker;
    __asm__ volatile("dmb" ::: "memory");
    if (base[0] != marker)
        return 0;

    for (size_t size = 1024u; size <= 16u * 1024u * 1024u; size <<= 1) {
        volatile uint32_t *probe = (volatile uint32_t *)(PSRAM_NOCACHE_BASE + size);
        uint32_t saved = *probe;
        *probe = ~marker;
        __asm__ volatile("dmb" ::: "memory");
        bool aliased = (base[0] != marker);
        *probe = saved;
        __asm__ volatile("dmb" ::: "memory");
        base[0] = marker;
        if (aliased)
            return size;
    }
    return 16u * 1024u * 1024u;
}

/* ---- console ---------------------------------------------------------- */

/*
 * The bootloader's own output goes through the same ring the kernel will use,
 * so there is one console for the whole boot rather than a UART that stops
 * working at handover. J4 is not wired on this bench in any case.
 */
static void ring_puts(const char *s)
{
    frank_ring_put(&FRANK_RING_SHARED->tx, (const uint8_t *)s,
                   (uint32_t)strlen(s));
}

/*
 * Hand-rolled number formatting instead of vsnprintf.
 *
 * ring_puts() worked and ring_printf() did not: the banner appeared and every
 * line after it vanished, which is a bootloader dying inside the C library
 * rather than doing its job. With stdio disabled the SDK's printf plumbing is
 * not something to rely on this early, and a bootloader has no business pulling
 * in a formatter to print six numbers.
 */
static void ring_putdec(uint32_t v)
{
    char buf[11];
    int i = 10;
    buf[i--] = 0;
    if (v == 0)
        buf[i--] = '0';
    while (v && i >= 0) {
        buf[i--] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    ring_puts(&buf[i + 1]);
}

static void ring_puthex32(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";
    char buf[11];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 8; i++)
        buf[2 + i] = hex[(v >> ((7 - i) * 4)) & 0xfu];
    buf[10] = 0;
    ring_puts(buf);
}

/* ---- handover --------------------------------------------------------- */

static uint32_t copy_payload(uint32_t flash_addr, uint32_t ram_addr,
                             const char *what)
{
    const payload_hdr_t *hdr = (const payload_hdr_t *)flash_addr;

    if (hdr->magic != PAYLOAD_MAGIC) {
        ring_puts("afboot: no ");
        ring_puts(what);
        ring_puts(" at ");
        ring_puthex32(flash_addr);
        ring_puts(" (magic ");
        ring_puthex32(hdr->magic);
        ring_puts(")\r\n");
        return 0;
    }
    if (hdr->length == 0 || hdr->length > 8u * 1024u * 1024u) {
        ring_puts("afboot: ");
        ring_puts(what);
        ring_puts(" length ");
        ring_putdec(hdr->length);
        ring_puts(" is implausible\r\n");
        return 0;
    }

    memcpy((void *)ram_addr, (const void *)(flash_addr + sizeof(payload_hdr_t)),
           hdr->length);
    ring_puts("afboot: ");
    ring_puts(what);
    ring_puts(" ");
    ring_putdec(hdr->length);
    ring_puts(" bytes -> ");
    ring_puthex32(ram_addr);
    ring_puts("\r\n");
    return hdr->length;
}

/*
 * Hand the kernel a clean NVIC.
 *
 * The pico-sdk leaves every interrupt at PICO_DEFAULT_IRQ_PRIORITY, which is
 * 0x80 -- and Linux sets SVCall to 0x80 as well. On ARMv7-M an exception
 * preempts only one of *strictly* higher priority, and the v7-M kernel runs
 * permanently inside SVCall (Handler mode), so an interrupt at equal priority
 * can never be taken.
 *
 * The result is a kernel that boots perfectly and then hangs in
 * calibrate_delay_converge waiting for a tick, with TIMER0 asserting, NVIC
 * ISER bit set, the IRQ pending in ISPR, PRIMASK and BASEPRI both clear, and
 * nothing wrong anywhere except two equal numbers:
 *
 *   SHPR2     = 0x80000000   SVCall priority 0x80
 *   NVIC IPR0 = 0x80808080   TIMER0 priority 0x80
 *
 * So disable everything, clear anything pending, and set every priority to 0.
 * Linux configures what it needs; it should not have to undo what we left.
 *
 * Core 1's NVIC is untouched: on ARMv8-M the NVIC is core-private, so this
 * cannot disturb the USB service running there.
 */
static void nvic_handover_reset(void)
{
    volatile uint32_t *nvic_icer = (volatile uint32_t *)0xE000E180u;
    volatile uint32_t *nvic_icpr = (volatile uint32_t *)0xE000E280u;
    volatile uint8_t  *nvic_ipr  = (volatile uint8_t  *)0xE000E400u;

    for (int i = 0; i < 8; i++) {
        nvic_icer[i] = 0xFFFFFFFFu;
        nvic_icpr[i] = 0xFFFFFFFFu;
    }
    for (int i = 0; i < 256; i++)
        nvic_ipr[i] = 0;

    __asm__ volatile("dsb; isb" ::: "memory");
}

/*
 * Enter the kernel.
 *
 * Not a function call: the kernel never returns, and it must find the stack and
 * registers exactly as the ARM boot protocol specifies. VTOR moves first so the
 * kernel's own vector table is in force before its first exception.
 *
 * __attribute__((noreturn)) and naked would be tidier, but this has to run with
 * the C environment already torn down enough that inline asm is clearer about
 * what actually reaches the CPU.
 */
static void __attribute__((noreturn)) enter_kernel(uint32_t entry, uint32_t dtb)
{
    __asm__ volatile(
        /*
         * Clear PRIMASK, do not set it.
         *
         * The obvious thing here is `cpsid i` to enter with interrupts off, and
         * it is fatal. Linux on ARMv7-M masks interrupts with BASEPRI, not
         * PRIMASK: local_irq_enable() writes BASEPRI and never touches PRIMASK,
         * so a PRIMASK set by the bootloader is never cleared by anyone and no
         * interrupt is ever delivered.
         *
         * The kernel then boots perfectly and hangs in calibrate_delay_converge
         * waiting for a jiffy that cannot arrive, with TIMER0 sitting there
         * asserting INTS=1 and INTR=1 that nothing will service.
         */
        "cpsie   i                  \n"
        "mov     r0, #0             \n"
        "mvn     r1, #0             \n" /* ~0: device-tree boot, no machine ID */
        "mov     r2, %[dtb]         \n"
        "bx      %[entry]           \n"
        :
        : [entry] "r"(entry | 1u), [dtb] "r"(dtb)
        : "r0", "r1", "r2", "memory");
    __builtin_unreachable();
}

/*
 * Enter the ROM's USB bootloader if Linux asked for it.
 *
 * The board's only recovery from a wedged debug port used to be holding BOOTSEL
 * while powering on -- a physical act, which makes the whole flash-and-test loop
 * depend on somebody being in the room. And a wedged debug port is not
 * hypothetical here: one stray store from user mode into the peripheral window
 * disables it until the next power-on, and openocd then reports "cannot read
 * IDR" on a chip that is otherwise running perfectly.
 *
 * So: Linux writes a magic word into SRAM and resets. SRAM survives a warm
 * reset, this runs before anything else, and the ROM's USB bootloader comes up
 * -- after which picotool can write flash without SWD being involved at all.
 *
 * The word is cleared before entering, so an interrupted attempt cannot leave
 * the board permanently in the bootloader.
 */
#define BOOTSEL_REQUEST_ADDR  0x2007dffcu
#define BOOTSEL_REQUEST_MAGIC 0xb0075e1fu

static void check_bootsel_request(void)
{
    volatile uint32_t *slot = (volatile uint32_t *)BOOTSEL_REQUEST_ADDR;

    if (*slot != BOOTSEL_REQUEST_MAGIC)
        return;

    *slot = 0;
    __asm__ volatile("dsb" ::: "memory");
    reset_usb_boot(0, 0);
}

int main(void)
{
    check_bootsel_request();

    clocks_bringup();

    /*
     * PSRAM before core 1, and the order is not negotiable.
     *
     * psram_init() takes the QMI out of XIP to configure chip select 1. Core 1
     * running TinyUSB is executing from flash, so its next instruction fetch
     * during that window has nowhere to come from and the core simply stops.
     * Measured exactly that way: core1_alive frozen at 0x1c9f75, the console
     * ring holding 211 undrained bytes, and only the first 25 -- one ring_puts
     * -- ever reaching the host.
     *
     * The cost is that a PSRAM failure happens before there is any console to
     * report it on, which the LED covers.
     */
    psram_init(FRANK_PSRAM_CS_PIN);
    size_t psram = psram_probe_bytes();

    /* Now the console. Everything below reports through it. */
    frank_core1_usb_start();

    /*
     * Wait for a terminal, with a timeout.
     *
     * Buffering alone does not solve this. The whole bootloader banner is about
     * 236 bytes, which fits inside TinyUSB's 256-byte TX FIFO, so it is
     * accepted and drained the instant the device enumerates -- long before
     * anyone opens the port -- and never applies the backpressure that would
     * have held it. Measured: ring head == tail == 0xec, everything gone.
     *
     * So wait for DTR rather than trying to buffer past it. Ten seconds is long
     * enough to open a terminal after flashing and short enough that an
     * unattended boot is not stuck; when it expires we boot anyway, because a
     * board that only starts when someone is watching is useless.
     */
    for (int i = 0; i < 1000 && !frank_console_ready(); i++)
        sleep_ms(10);

    ring_puts("\r\n=== afboot-rp2350 ===\r\n");
    uint32_t div = (qmi_hw->m[0].timing & QMI_M0_TIMING_CLKDIV_BITS)
                   >> QMI_M0_TIMING_CLKDIV_LSB;
    ring_puts("afboot: sys_clk ");
    ring_putdec(clock_get_hz(clk_sys) / 1000000u);
    ring_puts(" MHz, flash ");
    ring_putdec(div ? clock_get_hz(clk_sys) / div / 1000000u : 0u);
    ring_puts(" MHz\r\n");

    ring_puts("afboot: PSRAM ");
    ring_putdec((uint32_t)(psram / (1024u * 1024u)));
    ring_puts(" MB at ");
    ring_puthex32(PSRAM_BASE);
    ring_puts("\r\n");
    if (psram == 0) {
        ring_puts("afboot: no PSRAM, cannot continue\r\n");
        for (;;)
            tight_loop_contents();
    }

    uint32_t dtb_len = copy_payload(FLASH_DTB_ADDR, RAM_DTB_ADDR, "dtb");
    uint32_t kern_len = copy_payload(FLASH_KERNEL_ADDR, RAM_KERNEL_ADDR, "kernel");

    if (dtb_len) {
        /* FDT magic is big-endian, so byte-swap what we read little-endian. */
        uint32_t m = *(volatile uint32_t *)RAM_DTB_ADDR;
        m = __builtin_bswap32(m);
        if (m != FDT_MAGIC)
        {
            ring_puts("afboot: warning, DTB magic is ");
            ring_puthex32(m);
            ring_puts(" not ");
            ring_puthex32(FDT_MAGIC);
            ring_puts("\r\n");
        }
    }

    if (!kern_len) {
        ring_puts("afboot: no kernel to start; halting\r\n");
        for (;;)
            tight_loop_contents();
    }

    ring_puts("afboot: entry ");
    ring_puthex32(RAM_KERNEL_ADDR);
    ring_puts(", dtb ");
    ring_puthex32(RAM_DTB_ADDR);
    ring_puts(" -- starting Linux\r\n");

    /*
     * Open the link now that the flash copying is done.
     *
     * Core 1 leaves the link alone until this point. It shares the QMI with the
     * 2.79 MB kernel copy above, and running both at once wedged core 0 past
     * the point where SWD could even examine it.
     */
    frank_link_enable();

    /*
     * Wait for the master to say how big the disk is, before Linux asks.
     *
     * The kernel's driver reads the capacity exactly once, about 0.26 s after
     * handover, and declines to register permanently if it is still zero. Core
     * 1 retries the probe every 250 ms until the master answers, so without
     * this the two race and the machine boots without a disk -- reported as
     * "no storage offered by the master", which reads like a missing card.
     *
     * A second is far longer than the round trip needs and is only spent when
     * the master is genuinely absent, in which case there is no disk anyway.
     */
    for (int i = 0; i < 200 && FRANK_RING_SHARED->blk.capacity == 0; i++)
        sleep_ms(5);

    ring_puts("afboot: link capacity ");
    ring_putdec(FRANK_RING_SHARED->blk.capacity);
    ring_puts(" sectors\r\n");

    /* Let core 1 push the last of that out before the kernel takes over and
     * starts writing to the same ring. */
    sleep_ms(50);

    /*
     * VTOR is deliberately not touched.
     *
     * An earlier version pointed it at the kernel's entry address, which is
     * code and not a vector table. The kernel's own __v7m_setup installs its
     * vectors during head-nommu.S, which is why the QEMU boot wrapper does not
     * set VTOR either -- and that one boots.
     */
    nvic_handover_reset();
    enter_kernel(RAM_KERNEL_ADDR, RAM_DTB_ADDR);
}
