/*
 * USB CDC descriptors for the slave's console port.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Named so the port is identifiable on a bench that already has three CDC
 * devices on it -- two debug probes and whatever the master is doing. The
 * harness matches by USB serial number, and a device that calls itself "Pico"
 * like everything else is exactly the ambiguity tools/devices.py exists to
 * avoid.
 */

#include "tusb.h"

#define USB_VID   0x2E8A            /* Raspberry Pi */
#define USB_PID   0x000A            /* CDC-only device */
#define USB_BCD   0x0200

static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,
    /* Misc/IAD: a CDC device presents two interfaces that must be grouped. */
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

enum { ITF_NUM_CDC = 0, ITF_NUM_CDC_DATA, ITF_NUM_RESET, ITF_NUM_TOTAL };

/*
 * A vendor interface picotool recognises, so the board can be put into the ROM
 * bootloader over USB from any state.
 *
 * Without it the only way in is holding BOOTSEL while powering on -- a physical
 * act -- and that matters here because it is also the only recovery from a
 * debug port that has stopped answering, which on this board is a thing that
 * happens. `picotool reboot -f` then works whether Linux is running, wedged, or
 * the SWD link is dead, and picotool can write flash without SWD at all.
 *
 * Class/subclass/protocol are what picotool looks for; they come from the
 * pico-sdk's stdio_usb reset interface.
 */
#define RESET_INTERFACE_SUBCLASS    0x00
#define RESET_INTERFACE_PROTOCOL    0x01
#define RESET_REQUEST_BOOTSEL       0x01

/* A bare interface descriptor with no endpoints: picotool drives it entirely
 * with control transfers, so TUD_VENDOR_DESCRIPTOR (which insists on a bulk
 * pair) is the wrong shape and would waste two endpoints. */
#define TUD_RPI_RESET_DESC_LEN  9
#define TUD_RPI_RESET_DESCRIPTOR(_itfnum, _stridx) \
    9, TUSB_DESC_INTERFACE, _itfnum, 0, 0, TUSB_CLASS_VENDOR_SPECIFIC, \
    RESET_INTERFACE_SUBCLASS, RESET_INTERFACE_PROTOCOL, _stridx

#define EPNUM_CDC_NOTIF   0x81
#define EPNUM_CDC_OUT     0x02
#define EPNUM_CDC_IN      0x82

#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_RPI_RESET_DESC_LEN)

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
    TUD_RPI_RESET_DESCRIPTOR(ITF_NUM_RESET, 0),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_configuration;
}

static char const *string_desc_arr[] = {
    (const char[]){0x09, 0x04},     /* 0: English (0x0409) */
    "rh1.tech",                     /* 1: manufacturer     */
    "FRANK Linux console",          /* 2: product          */
    "FRANKLINUX01",                 /* 3: serial           */
    "FRANK Linux CDC",              /* 4: CDC interface    */
};

static uint16_t _desc_str[32];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    uint8_t chr_count;

    if (index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))
            return NULL;
        const char *str = string_desc_arr[index];
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31)
            chr_count = 31;
        for (uint8_t i = 0; i < chr_count; i++)
            _desc_str[1 + i] = str[i];
    }

    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}
