/*
 * link_blk.h - block requests over the inter-chip link.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The slave has no microSD connector; the card is on the master's SPI0. So a
 * block read on the slave is: Linux -> core 1 -> link -> master -> card, and
 * back. This header is the part on the wire, shared by both firmwares.
 *
 * Slave-initiated, which is the opposite of the bring-up firmware's strictly
 * master-led sequencing. The doorbells already support it: DB_SM is
 * slave-driven, so the slave raises it to mean "I have a request" and the
 * master polls for that rather than always going first.
 *
 * Sequence, with the master polling DB_SM:
 *
 *   slave                                   master
 *   -----                                   ------
 *   DB_SM = 1  --------------------------->  sees DB_SM
 *   arm RX                                   DB_MS = 1  ("ready")
 *   send request header (16 B)  ---------->  receive header
 *   [write: send data]          ---------->  [write: receive data, write card]
 *                               <----------  send reply header (8 B)
 *   [read: receive data]        <----------  [read: read card, send data]
 *   DB_SM = 0  --------------------------->  DB_MS = 0
 *
 * Everything is a fixed size the other end can predict, so neither side has to
 * negotiate a length mid-transfer.
 */

#ifndef LINK_BLK_H
#define LINK_BLK_H

#include <stdint.h>

#define LINK_BLK_MAGIC      0x4b4c4246u    /* "FBLK" */
#define LINK_BLK_SECTOR     512u

/* Sectors per request. 8 is 4 kB, which is one page-cache folio and keeps the
 * per-request overhead sane: F3 measured the master's control round-trip at
 * 2753 us with USB HID active, so a round-trip per *sector* would cap the disk
 * at about 360 sectors/s. Per 8 sectors it is eight times better, and the
 * kernel merges requests anyway. */
#define LINK_BLK_MAX_SECTORS 8u

enum {
    LINK_BLK_OP_READ  = 0,
    LINK_BLK_OP_WRITE = 1,
    LINK_BLK_OP_INFO  = 2,     /* ask the master for the card's capacity */
    LINK_BLK_OP_CON   = 3,     /* console: send output, collect keystrokes */
};

/*
 * Console payload sizes.
 *
 * The reply is a FIXED size regardless of how many keys are waiting, because
 * the receiver has to arm for an exact byte count before the sender starts and
 * the slave cannot know in advance how much the master will have to say. So the
 * master always sends the full key buffer and puts the valid count in the reply
 * header. Everything on this wire is a size the other end can predict.
 */
/*
 * Console bytes per transaction.
 *
 * 512 rather than 64, because full-screen programs change the arithmetic. A
 * scrolling shell emits a line at a time and 64 bytes was ample; vi, nano and
 * mc repaint the whole screen, which with attributes is several kB in one go.
 *
 * Every transaction costs a round trip -- 141 us idle, 2753 us with the
 * master's USB HID host running (F3) -- so at 64 bytes a 4 kB repaint is 64
 * transactions, a sixth of a second with a keyboard plugged in, and the editor
 * feels broken. At 512 it is eight, which is not noticeable.
 *
 * The ceiling is the master's receive buffer, which is sized for a full block
 * request at LINK_BLK_MAX_SECTORS * 512 = 4096 bytes.
 */
#define LINK_CON_MAX_TX   512u
#define LINK_CON_MAX_KEYS 32u      /* keystroke bytes, always sent in full */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t op;
    uint32_t lba;
    uint32_t count;            /* sectors; 0 for INFO */
} link_blk_req_t;

typedef struct __attribute__((packed)) {
    int32_t  status;           /* 0, or negative errno */
    uint32_t value;            /* INFO: sectors. CON: valid key bytes. */
} link_blk_rsp_t;

#endif /* LINK_BLK_H */
