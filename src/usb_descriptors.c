/*
 * This file is part of DeskHop (https://github.com/hrvach/deskhop).
 * Copyright (c) 2025 Hrvoje Cavrak
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * See the file LICENSE for the full license text.
 */

#include "usb_descriptors.h"
#include "main.h"
#include "tusb.h"

//--------------------------------------------------------------------+
// Device Descriptors
//--------------------------------------------------------------------+

                                        // https://github.com/raspberrypi/usb-pid
tusb_desc_device_t const desc_device_config = DEVICE_DESCRIPTOR(0x2e8a, 0x107c);

                                        // https://pid.codes/1209/C000/
tusb_desc_device_t const desc_device = DEVICE_DESCRIPTOR(0x1209, 0xc000);

// Invoked when received GET DEVICE DESCRIPTOR
// Application return pointer to descriptor
uint8_t const *tud_descriptor_device_cb(void) {
    if (global_state.config_mode_active)
        return (uint8_t const *)&desc_device_config;
    else
        return (uint8_t const *)&desc_device;
}

//--------------------------------------------------------------------+
// HID Report Descriptor
//--------------------------------------------------------------------+

// Relative mouse is used to overcome limitations of multiple desktops on MacOS and Windows

uint8_t const desc_hid_report[] = {TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_ID_KEYBOARD)),
                                   TUD_HID_REPORT_DESC_ABS_MOUSE(HID_REPORT_ID(REPORT_ID_MOUSE)),
                                   TUD_HID_REPORT_DESC_CONSUMER_CTRL(HID_REPORT_ID(REPORT_ID_CONSUMER)),
                                   TUD_HID_REPORT_DESC_SYSTEM_CONTROL(HID_REPORT_ID(REPORT_ID_SYSTEM))
                                   };

uint8_t const desc_hid_report_relmouse[] = {TUD_HID_REPORT_DESC_MOUSEHELP(HID_REPORT_ID(REPORT_ID_RELMOUSE))};

uint8_t const desc_hid_report_vendor[] = {TUD_HID_REPORT_DESC_VENDOR_CTRL(HID_REPORT_ID(REPORT_ID_VENDOR))};

/* The always-on channel. No report ID, so a report is exactly one
   CHANNEL_REPORT_SIZE packet carried opaquely. */
uint8_t const desc_hid_report_channel[] = {TUD_HID_REPORT_DESC_CHANNEL()};


// Invoked when received GET HID REPORT DESCRIPTOR
// Application return pointer to descriptor
// Descriptor contents must exist long enough for transfer to complete
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    switch(instance) {
        case ITF_NUM_HID:
            return desc_hid_report;
        case ITF_NUM_HID_REL_M:
            return desc_hid_report_relmouse;
        case ITF_NUM_HID_VENDOR:
            /* One interface slot, a different purpose in each mode: the config
               API in config mode, the channel in normal mode. The two never
               coexist - config mode reboots under a different identity. */
            return global_state.config_mode_active ? desc_hid_report_vendor
                                                   : desc_hid_report_channel;
        default:
            return desc_hid_report;
    }
}

bool tud_mouse_report(uint8_t mode, uint8_t buttons, int16_t x, int16_t y, int8_t wheel, int8_t pan) {
    mouse_report_t report = {.buttons = buttons, .wheel = wheel, .x = x, .y = y, .mode = mode, .pan = pan};
    uint8_t instance = ITF_NUM_HID;
    uint8_t report_id = REPORT_ID_MOUSE;

    if (mode == RELATIVE) {
        instance = ITF_NUM_HID_REL_M;
        report_id = REPORT_ID_RELMOUSE;
    }

    return tud_hid_n_report(instance, report_id, &report, sizeof(report));
}


//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+

// array of pointer to string descriptors
char const *string_desc_arr[] = {
    (const char[]){0x09, 0x04}, // 0: is supported language is English (0x0409)
    "Hrvoje Cavrak",            // 1: Manufacturer
#ifdef DH_DEV_NO_AUTH
    "DeskHop Switch (dev)",     // 2: Product, marked: channel authentication is compiled out
#else
    "DeskHop Switch",           // 2: Product
#endif
    "0",                        // 3: Serials, should use chip ID
    "DeskHop Helper",           // 4: Mouse Helper Interface
    "DeskHop Config",           // 5: Vendor Interface
    "DeskHop Disk",             // 6: Disk Interface
    "DeskHop Channel",          // 7: Helper Channel Interface
#ifdef DH_DEBUG
    "DeskHop Debug",            // 8: Debug Interface
#endif
};

// String Descriptor Index
enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_MOUSE,
    STRID_VENDOR,
    STRID_DISK,
    STRID_CHANNEL,
    STRID_DEBUG,
};

static uint16_t _desc_str[32];

// Invoked when received GET STRING DESCRIPTOR request
// Application return pointer to descriptor, whose contents must exist long enough for transfer to
// complete
uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;

    uint8_t chr_count;

    // 2 (hex) characters for every byte + 1 '\0' for string end
    static char serial_number[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1] = {0};

    if (!serial_number[0]) {
       pico_get_unique_board_id_string(serial_number, sizeof(serial_number));
    }

    if (index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        // Note: the 0xEE index string is a Microsoft OS 1.0 Descriptors.
        // https://docs.microsoft.com/en-us/windows-hardware/drivers/usbcon/microsoft-defined-usb-descriptors

        if (!(index < sizeof(string_desc_arr) / sizeof(string_desc_arr[0])))
            return NULL;

        const char *str = (index == STRID_SERIAL) ? serial_number : string_desc_arr[index];

        // Cap at max char
        chr_count = strlen(str);
        if (chr_count > 31)
            chr_count = 31;

        // Convert ASCII string into UTF-16
        for (uint8_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = str[i];
        }
    }

    // first byte is length (including header), second byte is string type
    _desc_str[0] = (TUSB_DESC_STRING << 8) | (2 * chr_count + 2);

    return _desc_str;
}

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+

#define EPNUM_HID        0x81
#define EPNUM_HID_REL_M  0x82
#define EPNUM_HID_VENDOR 0x83

/* The channel needs an interrupt OUT endpoint as well as IN, so that
   host-to-device traffic does not fall back to the shared control pipe. It
   takes the vendor IN address because it occupies the same interface slot, in
   the mode where the config interface does not exist. */
#define EPNUM_HID_CHANNEL_OUT 0x03
#define EPNUM_HID_CHANNEL_IN  EPNUM_HID_VENDOR

#define EPNUM_MSC_OUT    0x04
#define EPNUM_MSC_IN     0x84

#ifndef DH_DEBUG

#define ITF_NUM_TOTAL 3
#define ITF_NUM_TOTAL_CONFIG 4
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + 2 * TUD_HID_DESC_LEN + TUD_HID_INOUT_DESC_LEN)
#define CONFIG_TOTAL_LEN_CFG (TUD_CONFIG_DESC_LEN + 3 * TUD_HID_DESC_LEN + TUD_MSC_DESC_LEN)

#else
/* CDC uses 2 interfaces (control + data). In normal mode, place it right after
   the 3 HID interfaces (at 3, 4). In config mode, place it after HID_VENDOR (2)
   and MSC (3), so at 4, 5. */
#define ITF_NUM_CDC 3
#define ITF_NUM_CDC_CONFIG 4
#define ITF_NUM_TOTAL 5
#define ITF_NUM_TOTAL_CONFIG 6

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + 2 * TUD_HID_DESC_LEN + TUD_HID_INOUT_DESC_LEN + TUD_CDC_DESC_LEN)
#define CONFIG_TOTAL_LEN_CFG (TUD_CONFIG_DESC_LEN + 3 * TUD_HID_DESC_LEN + TUD_MSC_DESC_LEN + TUD_CDC_DESC_LEN)

#define EPNUM_CDC_NOTIF  0x85
#define EPNUM_CDC_OUT    0x06
#define EPNUM_CDC_IN     0x86

#endif


uint8_t const desc_configuration[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 500),

    // Interface number, string index, protocol, report descriptor len, EP In address, size & polling interval
    TUD_HID_DESCRIPTOR(ITF_NUM_HID,
                       STRID_PRODUCT,
                       HID_ITF_PROTOCOL_NONE,
                       sizeof(desc_hid_report),
                       EPNUM_HID,
                       LEGACY_EP_PACKET_SIZE,
                       1),

    TUD_HID_DESCRIPTOR(ITF_NUM_HID_REL_M,
                       STRID_MOUSE,
                       HID_ITF_PROTOCOL_NONE,
                       sizeof(desc_hid_report_relmouse),
                       EPNUM_HID_REL_M,
                       LEGACY_EP_PACKET_SIZE,
                       1),

    /* The always-on channel. Its own interface, carrying vendor-page
       collections only - sharing an interface with keyboard or mouse would
       make macOS require Input Monitoring for the whole node (ADR-0001).
       Interrupt IN and OUT, one 64-byte report per 1 ms frame each way. */
    TUD_HID_INOUT_DESCRIPTOR(ITF_NUM_HID_VENDOR,
                             STRID_CHANNEL,
                             HID_ITF_PROTOCOL_NONE,
                             sizeof(desc_hid_report_channel),
                             EPNUM_HID_CHANNEL_OUT,
                             EPNUM_HID_CHANNEL_IN,
                             CHANNEL_REPORT_SIZE,
                             1),
#ifdef DH_DEBUG
    // Interface number, string index, EP notification address and size, EP data address (out, in) and size.
    TUD_CDC_DESCRIPTOR(
        ITF_NUM_CDC, STRID_DEBUG, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, CFG_TUD_CDC_EP_BUFSIZE),
#endif
};

uint8_t const desc_configuration_config[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL_CONFIG, 0, CONFIG_TOTAL_LEN_CFG, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 500),

    // Interface number, string index, protocol, report descriptor len, EP In address, size & polling interval
    TUD_HID_DESCRIPTOR(ITF_NUM_HID,
                       STRID_PRODUCT,
                       HID_ITF_PROTOCOL_NONE,
                       sizeof(desc_hid_report),
                       EPNUM_HID,
                       LEGACY_EP_PACKET_SIZE,
                       1),

    TUD_HID_DESCRIPTOR(ITF_NUM_HID_REL_M,
                       STRID_MOUSE,
                       HID_ITF_PROTOCOL_NONE,
                       sizeof(desc_hid_report_relmouse),
                       EPNUM_HID_REL_M,
                       LEGACY_EP_PACKET_SIZE,
                       1),

    TUD_HID_DESCRIPTOR(ITF_NUM_HID_VENDOR,
                       STRID_VENDOR,
                       HID_ITF_PROTOCOL_NONE,
                       sizeof(desc_hid_report_vendor),
                       EPNUM_HID_VENDOR,
                       LEGACY_EP_PACKET_SIZE,
                       1),

    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC,
                       STRID_DISK,
                       EPNUM_MSC_OUT,
                       EPNUM_MSC_IN,
                       64),
#ifdef DH_DEBUG
    // Interface number, string index, EP notification address and size, EP data address (out, in) and size.
    TUD_CDC_DESCRIPTOR(
        ITF_NUM_CDC_CONFIG, STRID_DEBUG, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, CFG_TUD_CDC_EP_BUFSIZE),
#endif
};

/* A configuration descriptor whose declared wTotalLength disagrees with its
   real length enumerates in confusing, host-specific ways rather than failing
   outright, and adding an interface is exactly when that happens. Both
   variants are checked at build time, in every build configuration. */
TU_VERIFY_STATIC(sizeof(desc_configuration) == CONFIG_TOTAL_LEN,
                 "normal-mode descriptor length disagrees with CONFIG_TOTAL_LEN");
TU_VERIFY_STATIC(sizeof(desc_configuration_config) == CONFIG_TOTAL_LEN_CFG,
                 "config-mode descriptor length disagrees with CONFIG_TOTAL_LEN_CFG");
TU_VERIFY_STATIC(CHANNEL_REPORT_SIZE <= CFG_TUD_HID_EP_BUFSIZE,
                 "helper channel report exceeds the HID endpoint buffer");

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index; // for multiple configurations

    if (global_state.config_mode_active)
        return desc_configuration_config;
    else
        return desc_configuration;
}
