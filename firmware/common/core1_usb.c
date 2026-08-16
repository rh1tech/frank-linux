/*
 * core1_usb.c - USB CDC console service, running on core 1 beside Linux.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Core 0 runs Linux. Core 1 runs this: TinyUSB as a CDC device on the slave's
 * USB-C port (J9), moving bytes between the host and two SRAM rings. Linux
 * never touches the USB controller, which is the point -- there is no mainline
 * Linux driver for the RP2350's USB block.
 *
 * Everything here is __not_in_flash_func. Core 1 must not fetch from flash
 * while Linux is running: both cores would contend for the same QMI, and an XIP
 * miss costs about ten cycles per instruction (hw-findings F9). Running
 * entirely from SRAM also means core 1 keeps servicing USB while core 0 is
 * stalled on PSRAM, which is most of the time.
 */

#include <string.h>

#include "pico/multicore.h"
#include "pico/time.h"
#include "pico/bootrom.h"
#include "pico/stdlib.h"
#include "tusb.h"
/* usbd_class_driver_t and usbd_app_driver_get_cb() live here, not in tusb.h. */
#include "device/usbd_pvt.h"

#include "frank_ring.h"

/*
 * Claim the shared block and reset it.
 *
 * Called on core 0 before core 1 is launched, so there is no one to race with.
 * The magic is written last: until it is there, the Linux driver treats the
 * block as absent rather than trusting whatever the indices happen to contain
 * after a warm reset.
 */
void frank_ring_init(void)
{
    frank_ring_shared_t *s = FRANK_RING_SHARED;

    s->magic = 0;
    frank_ring_barrier();

    s->version = FRANK_RING_VERSION;
    s->core1_alive = 0;
    s->reserved = 0;
    s->tx.head = s->tx.tail = 0;
    s->rx.head = s->rx.tail = 0;

    frank_ring_barrier();
    s->magic = FRANK_RING_MAGIC;
    frank_ring_barrier();
}

/*
 * Console fan-out.
 *
 * Linux writes one stream, and two different things want to display it: the USB
 * CDC console and, over the link, the master's HDMI terminal. A byte can only
 * be taken out of the ring once, so it is taken once into this buffer and each
 * consumer reads it at its own pace.
 *
 * A consumer only holds data back while it is actually there. USB counts as
 * present when it is enumerated, the link when the master has answered. An
 * absent consumer has its tail snapped forward, so an unplugged USB cable
 * cannot stop the HDMI console and a missing master cannot stop USB. Without
 * that, whichever one is not connected fills the buffer and stalls the other.
 *
 * When NEITHER is present the ring is not drained at all. That is not the same
 * as snapping both tails: draining into a buffer nobody reads throws the data
 * away, and the data in question is the boot log. F12 is exactly this failure
 * -- afboot printed its banner before USB enumerated, the bytes went into a
 * FIFO no one was attached to, and the screen stayed blank. Holding it in the
 * ring means it is still there when a console finally appears.
 */
/* Large enough to hold a full-screen repaint without chopping it into
 * link transactions smaller than LINK_CON_MAX_TX. Power of two: FAN_MASK. */
#define FAN_SIZE 4096u
#define FAN_MASK (FAN_SIZE - 1u)

static uint8_t fan[FAN_SIZE];
static uint32_t fan_head, fan_usb_tail, fan_link_tail;

static inline uint32_t fan_used(uint32_t tail)
{
    return (fan_head - tail) & FAN_MASK;
}

static uint32_t fan_take(uint32_t *tail, uint8_t *dst, uint32_t max)
{
    uint32_t n = fan_used(*tail);

    if (n > max)
        n = max;
    for (uint32_t i = 0; i < n; i++)
        dst[i] = fan[(*tail + i) & FAN_MASK];
    *tail = (*tail + n) & FAN_MASK;
    return n;
}

/* Defined by the link service when one is linked in; see core1_blk.c. */
__attribute__((weak)) bool frank_link_console_up(void) { return false; }

/* Bytes waiting for the master's terminal, and keystrokes coming back. Called
 * from the link service, on the same core, so no locking is needed. */
uint32_t frank_console_take(uint8_t *dst, uint32_t max)
{
    return fan_take(&fan_link_tail, dst, max);
}

void frank_console_feed(const uint8_t *src, uint32_t n)
{
    frank_ring_put(&FRANK_RING_SHARED->rx, src, n);
}

static void __not_in_flash_func(fan_fill)(void)
{
    frank_ring_shared_t *s = FRANK_RING_SHARED;
    uint8_t buf[64];
    bool usb_up = tud_ready();
    bool link_up = frank_link_console_up();
    uint32_t held = 0;

    if (!usb_up && !link_up)
        return;

    if (!usb_up)
        fan_usb_tail = fan_head;
    else
        held = fan_used(fan_usb_tail);

    if (!link_up)
        fan_link_tail = fan_head;
    else if (fan_used(fan_link_tail) > held)
        held = fan_used(fan_link_tail);

    uint32_t room = FAN_MASK - held;
    if (room > sizeof(buf))
        room = sizeof(buf);
    if (!room)
        return;

    uint32_t n = frank_ring_get(&s->tx, buf, room);
    for (uint32_t i = 0; i < n; i++)
        fan[(fan_head + i) & FAN_MASK] = buf[i];
    fan_head = (fan_head + n) & FAN_MASK;
}

static void __not_in_flash_func(service_once)(void)
{
    frank_ring_shared_t *s = FRANK_RING_SHARED;
    uint8_t buf[64];

    tud_task();

    fan_fill();

    /*
     * Linux -> host.
     *
     * Gated on tud_ready() -- the device is enumerated -- and NOT on
     * tud_cdc_connected(), which reflects DTR. macOS asserts DTR briefly while
     * enumerating, and treating that as a reader drained the boot banner into a
     * port nobody had opened yet (hw-findings F12). The bootloader's output
     * vanished exactly that way.
     *
     * With tud_ready() plus tud_cdc_write_available() the behaviour is right in
     * every case that matters: unplugged, nothing is drained; enumerated but no
     * terminal open, the endpoint FIFO fills, available goes to zero and the
     * ring simply holds the data; terminal attached and reading, it drains.
     * That is real backpressure rather than a timing race, and it means a
     * kernel boot log written before anyone attaches is still there when they do.
     */
    if (tud_ready()) {
        uint32_t room = tud_cdc_write_available();
        if (room > sizeof(buf))
            room = sizeof(buf);
        if (room) {
            uint32_t n = fan_take(&fan_usb_tail, buf, room);
            if (n) {
                tud_cdc_write(buf, n);
                tud_cdc_write_flush();
            }
        }
    }

    /* Host -> Linux. */
    if (tud_cdc_available()) {
        uint32_t room = frank_ring_space(&s->rx);
        if (room > sizeof(buf))
            room = sizeof(buf);
        if (room) {
            uint32_t n = tud_cdc_read(buf, room);
            if (n)
                frank_ring_put(&s->rx, buf, n);
        }
    }

    /* A liveness counter rather than a flag: the Linux side can tell "core 1
     * started once and then wedged" from "core 1 is running", which a boolean
     * cannot. */
    s->core1_alive++;

    /*
     * Publish a *debounced* DTR for core 0; see frank_console_ready().
     *
     * Raw DTR is not usable as "a terminal is open". macOS asserts it
     * transiently while enumerating, and measured on this bench that blip was
     * enough to convince the bootloader a reader had arrived: it printed its
     * whole banner into a port nobody had opened, the 236 bytes fitted inside
     * TinyUSB's 256-byte FIFO so no backpressure ever built up, and the ring
     * came back empty (head == tail == 0xec) with nothing on screen.
     *
     * A quarter of a second of continuous assertion is far longer than the
     * enumeration blip and far shorter than a human opening a terminal.
     */
    static uint64_t dtr_since;
    if (tud_cdc_connected()) {
        uint64_t now = time_us_64();
        if (dtr_since == 0)
            dtr_since = now;
        s->reserved = (now - dtr_since >= 250000u) ? 1u : 0u;
    } else {
        dtr_since = 0;
        s->reserved = 0;
    }
}

/*
 * Is a terminal actually open?
 *
 * DTR, sampled on core 1 and published for core 0 to read. Core 0 cannot call
 * tud_cdc_connected() itself: TinyUSB's state belongs to core 1 and is not safe
 * to touch from the other core.
 */
/* Shared by afboot and the hardware tests: one console write path rather than a
 * copy in each. */
void frank_ring_puts(const char *str)
{
    const uint8_t *p = (const uint8_t *)str;
    uint32_t len = 0;
    while (p[len]) len++;
    frank_ring_put(&FRANK_RING_SHARED->tx, p, len);
}

/*
 * The reset interface picotool talks to.
 *
 * A class driver, not just a control callback: TinyUSB opens every interface in
 * the configuration descriptor with a driver, and an interface no driver claims
 * makes SET_CONFIGURATION fail -- the device then never enumerates at all. That
 * is a bad way to find out, because the console it takes down is the one you
 * would have used to debug it.
 *
 * The point of all this is that the board can be put into the ROM bootloader
 * over its own USB, from any state, without SWD and without somebody holding a
 * button -- which is also the only recovery from a debug port that has stopped
 * answering, and this board's does.
 */
#define RESET_INTERFACE_SUBCLASS  0x00
#define RESET_INTERFACE_PROTOCOL  0x01
#define RESET_REQUEST_BOOTSEL     0x01

static uint8_t reset_itf_num;

static void resetd_init(void) { }

static void resetd_reset(uint8_t rhport)
{
    (void)rhport;
    reset_itf_num = 0;
}

static uint16_t resetd_open(uint8_t rhport, tusb_desc_interface_t const *itf,
                            uint16_t max_len)
{
    (void)rhport;

    TU_VERIFY(itf->bInterfaceClass == TUSB_CLASS_VENDOR_SPECIFIC &&
              itf->bInterfaceSubClass == RESET_INTERFACE_SUBCLASS &&
              itf->bInterfaceProtocol == RESET_INTERFACE_PROTOCOL, 0);
    TU_VERIFY(max_len >= sizeof(tusb_desc_interface_t), 0);

    reset_itf_num = itf->bInterfaceNumber;
    return sizeof(tusb_desc_interface_t);
}

static bool resetd_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                   tusb_control_request_t const *request)
{
    if (stage != CONTROL_STAGE_SETUP)
        return true;
    if (request->wIndex != reset_itf_num)
        return false;

    if (request->bRequest == RESET_REQUEST_BOOTSEL) {
        tud_control_status(rhport, request);
        reset_usb_boot(0, 0);           /* does not return */
    }
    return false;
}

static bool resetd_xfer_cb(uint8_t rhport, uint8_t ep_addr,
                           xfer_result_t result, uint32_t xferred_bytes)
{
    (void)rhport; (void)ep_addr; (void)result; (void)xferred_bytes;
    return true;                        /* no endpoints; control transfers only */
}

static const usbd_class_driver_t reset_driver = {
#if CFG_TUSB_DEBUG >= 2
    .name             = "RESET",
#endif
    .init             = resetd_init,
    .reset            = resetd_reset,
    .open             = resetd_open,
    .control_xfer_cb  = resetd_control_xfer_cb,
    .xfer_cb          = resetd_xfer_cb,
    .sof              = NULL,
};

const usbd_class_driver_t *usbd_app_driver_get_cb(uint8_t *driver_count)
{
    *driver_count = 1;
    return &reset_driver;
}

bool frank_console_ready(void)
{
    return FRANK_RING_SHARED->reserved != 0;
}

/*
 * Optional block service, linked in only where storage is wanted. Weak symbols
 * so the console-only builds -- the hardware tests, and afboot before the
 * kernel exists -- do not have to drag in the link and its PIO programs.
 */
__attribute__((weak)) void frank_blk_init(void) { }
__attribute__((weak)) void frank_blk_service(void) { }

void __not_in_flash_func(core1_usb_main)(void)
{
    tusb_init();
    frank_blk_init();
    for (;;) {
        service_once();
        frank_blk_service();
    }
}

/*
 * Start the service. Call from core 0 before handing over to Linux.
 *
 * The ring is initialised first and the magic is already in place by the time
 * core 1 runs, so core 1 never has to wait for it.
 */
void frank_core1_usb_start(void)
{
    frank_ring_init();
    multicore_launch_core1(core1_usb_main);
}
