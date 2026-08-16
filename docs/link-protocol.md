# The inter-chip link, and what Linux does with it

## What changed

The plan had Phase 5 carrying the **console** over the link, because the slave
had no other way to talk. It does now: core 1 serves USB CDC on J9 and Linux
sees a ring (F12, F13). That is strictly better — it does not depend on the
master being alive, and it survives the master being reflashed.

So the link's remaining jobs are the ones only the master can do:

| Job | Why the link | Phase |
|---|---|---|
| **microSD** | The slave has no SD connector at all. The card is on the master's SPI0 (GPIO 4–7). | 5 |
| **HDMI terminal** | HSTX is wired to the master's GPIO 12–19. | 6 |
| **USB keyboard** | The HID host is the master's port J8. | 6 |
| **Audio** | The I2S DAC is on the master's GPIO 9–11. | 7 |

Storage is the one that changes what Linux *is*. Everything so far has run from
an initramfs in the same 8 MB that holds the kernel, which F8 measured as the
binding constraint. A real root filesystem on SD moves userspace out of RAM
entirely and leaves the 8 MB for running programs.

## Shape

Core 1 already exists, is SRAM-resident, and runs a service loop. It gains the
link the same way it has USB: the kernel never touches PIO, DMA or the wire.

```
   Linux (core 0)          core 1                    master
  +----------------+   +--------------+          +------------------+
  | frank-ring tty |<->| console rings|<-- USB ->| (host terminal)  |
  | frank-blk      |<->| block ring   |<=========| SD card on SPI0  |
  | frank-term     |<->| term rings   |   link   | HDMI + keyboard  |
  +----------------+   +--------------+  96MiB/s +------------------+
        PSRAM             SRAM
```

Three transports, one service loop, one shared block in SRAM. The console rings
are already built and proven; the other two reuse the same discipline.

## Why the block channel is not a byte ring

The console is a stream and a ring fits it. Storage is request/response with
bulk payloads, and copying 512-byte sectors through a 2 kB ring twice would be
wasteful when core 1 can reach the kernel's buffers directly — PSRAM is visible
to both cores, and CPU/DMA coherence there is already confirmed (F9).

So the block channel is a **descriptor plus a doorbell**, and the data never
enters the ring at all:

```c
typedef struct {
    volatile uint32_t seq;     /* Linux increments to submit          */
    volatile uint32_t done;    /* core 1 copies seq here when finished*/
    volatile uint32_t op;      /* 0 = read, 1 = write                 */
    volatile uint32_t lba;
    volatile uint32_t count;   /* 512-byte sectors                    */
    volatile uint32_t addr;    /* buffer, physical, in PSRAM          */
    volatile int32_t  status;  /* 0, or negative errno                */
} frank_blk_req_t;
```

Submission is: fill the fields, barrier, bump `seq`. Completion is: `done ==
seq`. One producer and one consumer again, so still no locks and still no
atomics — which remains necessary rather than elegant, because with
`ARM_NO_EXCLUSIVES` the kernel's atomics are interrupt-masked and protect
nothing against core 1.

`status` is signed and separate from `done` deliberately: a failed read must be
distinguishable from a completed one, and folding them into a single word makes
"finished" and "worked" the same bit.

## What has to be true before this works

1. **Core 1 must run the link from SRAM.** `link_bus.c` in the FRANK firmware is
   flash-resident. Core 1 executing from flash dies whenever anything
   reconfigures the QMI — that is F13(4), and it cost a debugging session. The
   link code has to be `__not_in_flash_func` throughout, and its PIO programs
   loaded before Linux starts.

2. **The master must be able to initiate.** The existing protocol is strictly
   master-led: `DB_MS` means "master ready" and the master polls. A block
   completion is the slave answering, which the doorbell handshake already
   supports, but an unsolicited keypress from the master is not something the
   current sequencing has a place for. `DB_SM` is slave-driven, so the natural
   arrangement is for the master to poll it for attention.

3. **Latency budget.** F3 measured the master's control round-trip at 141 µs
   idle and **2753 µs with USB HID active**. A block read that does a control
   round-trip per sector would manage 360 sectors/s with a keyboard attached.
   Requests must therefore carry many sectors per round-trip, and the transport
   must be a streaming ring rather than request/response per unit — the same
   conclusion F3 reached for the console.

## Order of work

1. Core 1 link service, SRAM-resident, echoing over the link — testable as a
   `hwtests/` pair with no Linux involved, the same way `usbring` was.
2. `frank-blk` in Linux, backed by a RAM disk on the master, so the block path
   is proven before the SD card is in it.
3. Swap the RAM disk for the real card; move the rootfs onto it.
4. Phase 6 adds the terminal rings and the master runs Protea.
