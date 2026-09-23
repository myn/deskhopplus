/*
 * This file is part of DeskHop (https://github.com/hrvach/deskhop).
 * Copyright (c) 2025 Hrvoje Cavrak
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * See the file LICENSE for the full license text.
 * Modified by Derek Reynolds, 2026, for deskhopplus.
 */

#include "main.h"

#include "pio_usb_ll.h"

_Static_assert(MAX_DEVICES <= CFG_TUH_DEVICE_MAX,
               "MAX_DEVICES must not exceed CFG_TUH_DEVICE_MAX");

/* ================================================== *
 * ===========  TinyUSB Device Callbacks  =========== *
 * ================================================== */

/* Invoked when device is mounted */
void tud_mount_cb(void) {
    discard_queued_host_reports();
    global_state.tud_connected = true;
    tud_mouse_report_reset(global_state.pointer_x, global_state.pointer_y);

    /* A modifier is held by the host, not by the board: the OS believes the
       last report it was given until a later one says otherwise. Any reset
       that skips the release therefore strands a modifier the board cannot
       see - reset_usb_boot on the flash path returns immediately, and a
       watchdog reset or a power cut announce nothing by construction. That
       was #138, a phantom GUI key after a flash.

       Enumeration is the one moment every one of those has in common, and it
       is the moment the host's own belief is empty, so an empty report is
       what agrees with it. Not every mount follows a reboot - a replug or a
       host resume lands here with RAM intact and a key possibly still held -
       and clearing unconditionally is still right: the host that just
       enumerated holds nothing either way, and both state arrays are
       overwritten wholesale by the next report from their source
       (update_kbd_state, update_remote_kbd_state), so a key held across the
       mount is back the moment its keyboard speaks again. handle_output_select_msg
       already releases on the same reasoning.

       One report per attach, and it drains in config mode too - that identity
       keeps ITF_NUM_HID. */
    release_all_keys(&global_state);
}

/* Invoked when device is unmounted */
void tud_umount_cb(void) {
    global_state.tud_connected = false;
    discard_queued_host_reports();

    /* The channel went with it. Config mode reboots the device under a
       different USB identity, so this is also the ordinary path in and out of
       it — the helper reconnects and says hello again.

       channel_link_lost and not channel_init, which would take an open
       pairing window with it. That was #100: 5617314 wrote the fix and left
       it with no caller, so the window kept dying to a bus reset the header
       already said it survived. Not re-reading flash here costs nothing —
       every path that changes the registration in the running config tells the
       pairing module itself, and the board's identity is drawn once for its
       whole life. Config mode is decided at boot, where channel_init still
       runs. */
    channel_link_lost();
}

#ifdef DH_DEBUG_CDC_FLASH
void tud_cdc_rx_cb(uint8_t itf) {
    char buf[64];
    uint32_t count = tud_cdc_n_available(itf);

    if (count == 0)
        return;

    if (count > sizeof(buf))
        count = sizeof(buf);

    tud_cdc_n_read(itf, buf, count);

    if (count >= 5 && memcmp(buf, "flash", 5) == 0) {
        reset_usb_boot(0, 0);
    }
}
#endif

/* ================================================== *
 * ===============  USB HOST Section  =============== *
 * ================================================== */

/* The device itself, before any class driver claims an interface (#102). */
void tuh_mount_cb(uint8_t dev_addr) {
    cursor_trace_event(&global_state, DH_CURSOR_TRACE_DEV_MOUNT, dev_addr, 0, 0, 0, 0);
}

void tuh_umount_cb(uint8_t dev_addr) {
    cursor_trace_event(&global_state, DH_CURSOR_TRACE_DEV_UNMOUNT, dev_addr, 0, 0, 0, 0);
}

/*
 * Whether the root port holds a device, as the library sees it: set when an
 * idle line is first seen, cleared only by its own two-sample SE0 check at
 * the top of a frame. Not a raw pin read — every SOF ends in a few hundred
 * nanoseconds of SE0, so a read from this core would say "unplugged" several
 * times a second in exactly the wedge this exists to catch.
 */
bool usb_host_attached(void) {
    return PIO_USB_ROOT_PORT(0)->connected;
}

/* Every address TinyUSB hands out: the end devices and, after them, the hub
   (usbh.c TOTAL_DEVICES). A mounted hub with nothing behind it counts, so an
   empty hub is not pulled forever. */
bool usb_host_any_mounted(void) {
    for (uint8_t addr = 1; addr <= CFG_TUH_DEVICE_MAX + CFG_TUH_HUB; ++addr)
        if (tuh_mounted(addr))
            return true;
    return false;
}

/*
 * Emulate a cable pull on the root port (#102).
 *
 * A pull is SE0 on the bus, which the SOF handler's connection check reads
 * as a disconnect: TinyUSB tears down whatever half-enumerated device it
 * holds (dev0 included), and the next idle frame is a fresh attach with the
 * full reset-and-enumerate that follows one. The port reset already drives
 * SE0; it also marks the root suspended, which is what keeps the check from
 * running during TinyUSB's own reset. Clearing that flag while the line is
 * held low is the whole trick. Held for 20 ms: past the 10 ms a device needs
 * to take it as a reset, and many frames more than the check needs.
 *
 * The root is parked first. The reset drives the pins through the PIO
 * transmit state machine, and the SOF handler on the other core is using
 * that machine every frame while the root is live; TinyUSB only ever resets
 * a root it has just seen attach, which is still suspended. Two frames
 * parked is one in flight finishing, with margin.
 */
void usb_host_replug(void) {
    root_port_t *root = PIO_USB_ROOT_PORT(0);
    root->suspended = true;
    busy_wait_ms(2);
    pio_usb_host_port_reset_start(0);
    root->suspended = false;
    busy_wait_ms(20);
    pio_usb_host_port_reset_end(0);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    uint8_t itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

    cursor_trace_event(&global_state, DH_CURSOR_TRACE_HID_UNMOUNT, dev_addr, instance,
                       itf_protocol, 0, 0);
    if (dev_addr > MAX_DEVICES || instance >= MAX_INTERFACES)
        return;

    hid_interface_t *iface = &global_state.iface[dev_addr-1][instance];

    switch (itf_protocol) {
        case HID_ITF_PROTOCOL_KEYBOARD:
            global_state.keyboard_connected = false;
            break;

        case HID_ITF_PROTOCOL_MOUSE:
            global_state.mouse_connected = false;
            break;
    }

    /* Also clear the interface structure, otherwise plugging something else later
       might be a fun (and confusing) experience */
    memset(iface, 0, sizeof(hid_interface_t));
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len) {
    uint8_t itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

    /* A mount that silently does not take is #102's whole symptom, so every
       exit from here leaves a record (see DH_CURSOR_TRACE_HID_MOUNT). */
    if (dev_addr > MAX_DEVICES || instance >= MAX_INTERFACES) {
        cursor_trace_event(&global_state, DH_CURSOR_TRACE_HID_MOUNT, dev_addr, instance,
                           itf_protocol, 0, 3);
        return;
    }

    /* Get interface information */
    hid_interface_t *iface = &global_state.iface[dev_addr-1][instance];

    iface->protocol = tuh_hid_get_protocol(dev_addr, instance);

    /* Parse the report descriptor into our internal structure. */
    parse_report_descriptor(iface, desc_report, desc_len);

    switch (itf_protocol) {
        case HID_ITF_PROTOCOL_KEYBOARD:
            if (global_state.config.enforce_ports && BOARD_ROLE == OUTPUT_B)
                return;

            if (global_state.config.force_kbd_boot_protocol)
                tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_BOOT);

            /* Keeping this is required for setting leds from device set_report callback */
            global_state.kbd_dev_addr       = dev_addr;
            global_state.kbd_instance       = instance;
            global_state.keyboard_connected = true;
            break;

        case HID_ITF_PROTOCOL_MOUSE:
            if (global_state.config.enforce_ports && BOARD_ROLE == OUTPUT_A)
                return;

            if (global_state.config.force_mouse_boot_mode) {
                /* User requested boot mode - simpler protocol for compatibility.
                   Note: many mice still send wheel data even in boot mode. */
                tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_BOOT);
            } else {
                /* Switch to using report protocol instead of boot, it's more complicated but
                   at least we get all the information we need (looking at you, mouse wheel) */
                if (tuh_hid_get_protocol(dev_addr, instance) == HID_PROTOCOL_BOOT) {
                    tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_REPORT);
                }
            }
            global_state.mouse_connected = true;
            break;

        case HID_ITF_PROTOCOL_NONE:
            break;
    }

    /* Also set mouse_connected if report descriptor contains mouse, even if interface
       protocol says keyboard. This handles composite devices like QMK. */
    if (iface->mouse.is_found) {
        global_state.mouse_connected = true;
    }

    /* Flash local led to indicate a device was connected */
    blink_led(&global_state);

    /* Also signal the other board to flash LED, to enable easy verification if serial works */
    (void)send_value(ENABLE, FLASH_LED_MSG);

    /* Kick off the report querying */
    const bool polling = tuh_hid_receive_report(dev_addr, instance);
    cursor_trace_event(&global_state, DH_CURSOR_TRACE_HID_MOUNT, dev_addr, instance,
                       itf_protocol,
                       (global_state.keyboard_connected ? 1u : 0u) |
                           (global_state.mouse_connected ? 2u : 0u),
                       polling ? 1 : 2);
}

/* Invoked when received report from device via interrupt endpoint */
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

    if (dev_addr > MAX_DEVICES || instance >= MAX_INTERFACES)
        return;

    hid_interface_t *iface = &global_state.iface[dev_addr-1][instance];

    /* Calculate a device index that distinguishes between different devices
       while staying within the bounds of MAX_DEVICES.

       Device index assignment:
       - 0: Primary keyboard (the one set in tuh_hid_mount_cb)
       - 1: Mouse devices
       - MAX_DEVICES-2: Secondary keyboards (e.g., wireless keyboard through unified dongle)
       - (dev_addr-1) % (MAX_DEVICES-1): Other devices

       Note: Slot MAX_DEVICES-1 is reserved for the remote device (used in handle_keyboard_uart_msg) */
    uint8_t device_idx;

    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
        if (dev_addr == global_state.kbd_dev_addr && instance == global_state.kbd_instance) {
            /* Primary keyboard */
            device_idx = 0;
        } else {
            /* Secondary keyboard (e.g., wireless keyboard through unified dongle) */
            device_idx = (MAX_DEVICES - 2);
        }
    } else if (itf_protocol == HID_ITF_PROTOCOL_MOUSE) {
        /* Mouse devices */
        device_idx = 1;
    } else {
        /* Other devices */
        device_idx = (dev_addr - 1) % (MAX_DEVICES - 1);
    }

    if (iface->uses_report_id || itf_protocol == HID_ITF_PROTOCOL_NONE) {
        uint8_t report_id = 0;

        if (iface->uses_report_id)
            report_id = report[0];

        if (report_id < MAX_REPORTS) {
            process_report_f receiver = iface->report_handler[report_id];

            if (receiver != NULL)
                receiver((uint8_t *)report, len, device_idx, iface);
        }
    }
    else if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
        process_keyboard_report((uint8_t *)report, len, device_idx, iface);
    }
    else if (itf_protocol == HID_ITF_PROTOCOL_MOUSE) {
        process_mouse_report((uint8_t *)report, len, device_idx, iface);
    }

    /* Continue requesting reports */
    tuh_hid_receive_report(dev_addr, instance);
}

/* Set protocol in a callback. This is tied to an interface, not a specific report ID */
void tuh_hid_set_protocol_complete_cb(uint8_t dev_addr, uint8_t idx, uint8_t protocol) {
    if (dev_addr > MAX_DEVICES || idx >= MAX_INTERFACES)
        return;

    hid_interface_t *iface = &global_state.iface[dev_addr-1][idx];
    iface->protocol = protocol;
}
