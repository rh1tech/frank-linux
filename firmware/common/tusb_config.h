/*
 * TinyUSB configuration: CDC device on core 1.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Device, not host. The FRANK firmware's existing tusb_config.h files are all
 * host configurations for the USB HID keyboard on the master; this is the other
 * direction, on the slave's J9 port, and the two cannot share a config.
 */

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#define CFG_TUSB_MCU                OPT_MCU_RP2040   /* RP2350 uses the same driver */
#define CFG_TUSB_OS                 OPT_OS_PICO
#define CFG_TUSB_RHPORT0_MODE       OPT_MODE_DEVICE

/*
 * Descriptors and endpoint buffers must live in memory the USB controller can
 * reach, and the service runs from SRAM anyway (core 1 must not fetch from
 * flash while Linux is running -- both cores would contend for the same QMI).
 */
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN          __attribute__((aligned(4)))

#define CFG_TUD_ENABLED             1
#define CFG_TUD_CDC                 1
#define CFG_TUD_MSC                 0
#define CFG_TUD_HID                 0
#define CFG_TUD_MIDI                0
#define CFG_TUD_VENDOR              0

/* 256-byte FIFOs each way. The SRAM rings behind them are 2 kB, so these only
 * need to cover one service pass rather than buffer the console. */
#define CFG_TUD_CDC_RX_BUFSIZE      256
#define CFG_TUD_CDC_TX_BUFSIZE      256
#define CFG_TUD_CDC_EP_BUFSIZE      64

#endif
