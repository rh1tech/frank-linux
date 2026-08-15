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

    /* Linux -> host. Only move what the host can currently accept, so a
     * disconnected or unread port applies backpressure through the ring
     * instead of silently dropping the kernel's output. */
    if (tud_cdc_connected()) {
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
