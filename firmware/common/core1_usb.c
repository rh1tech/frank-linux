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
#include "pico/stdlib.h"
#include "tusb.h"

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

static void __not_in_flash_func(service_once)(void)
{
    frank_ring_shared_t *s = FRANK_RING_SHARED;
    uint8_t buf[64];

    tud_task();

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
            uint32_t n = frank_ring_get(&s->tx, buf, room);
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
bool frank_console_ready(void)
{
    return FRANK_RING_SHARED->reserved != 0;
}

void __not_in_flash_func(core1_usb_main)(void)
{
    tusb_init();
    for (;;)
        service_once();
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
