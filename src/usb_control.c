/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Derek Reynolds */

#include "main.h"
#include "core/dh_keyboard_output.h"

/* Reports from the previous USB enumeration must not reach a new host state. */
void discard_queued_host_reports(void) {
    hid_keyboard_report_t keys;
    mouse_report_t mouse;
    hid_generic_pkt_t packet;
    while (queue_try_remove(&global_state.kbd_queue, &keys)) {}
    while (queue_try_remove(&global_state.mouse_queue, &mouse)) {}
    while (queue_try_remove(&global_state.hid_queue_out, &packet)) {}
}

void tud_hid_set_protocol_cb(uint8_t instance, uint8_t protocol) {
    if (instance == ITF_NUM_HID_REL_M && protocol == HID_PROTOCOL_BOOT)
        tud_mouse_report_reset(global_state.pointer_x, global_state.pointer_y);
}

/* Invoked when we get GET_REPORT control request.
 * We are expected to fill buffer with the report content, update reqlen
 * and return its length. We return 0 to STALL the request. */
uint16_t tud_hid_get_report_cb(uint8_t instance,
                               uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer,
                               uint16_t request_len) {
    if (instance == ITF_NUM_HID_REL_M && report_id == 0
        && report_type == HID_REPORT_TYPE_INPUT
        && tud_hid_n_get_protocol(instance) == HID_PROTOCOL_BOOT && request_len >= 3) {
        memset(buffer, 0, 3); /* Relative mouse has no stationary position to report. */
        return 3;
    }

    if (instance != ITF_NUM_HID)
        return 0;

    bool boot = tud_hid_n_get_protocol(instance) == HID_PROTOCOL_BOOT;
    if (report_id != (boot ? 0 : REPORT_ID_KEYBOARD))
        return 0;

    if (report_type == HID_REPORT_TYPE_OUTPUT && request_len >= 1) {
        buffer[0] = global_state.keyboard_leds_desired[BOARD_ROLE];
        return 1;
    }

    if (report_type == HID_REPORT_TYPE_INPUT && request_len >= sizeof(hid_keyboard_report_t)) {
        hid_keyboard_report_t report = {0};
        if (CURRENT_BOARD_IS_ACTIVE_OUTPUT) {
            hid_keyboard_report_t combined;
            combine_kbd_states(&global_state, &combined);
            dh_keyboard_output_prepare((const uint8_t *)&combined, DH_KEYBOARD_PHYSICAL,
                                       &global_state.config.output[BOARD_ROLE].keymap,
                                       global_state.config.output[BOARD_ROLE].swap_ctrl_gui,
                                       (uint8_t *)&report);
        }
        report.reserved = 0;
        memcpy(buffer, &report, sizeof(report));
        return sizeof(report);
    }

    return 0;
}

/**
 * Computer controls our LEDs by sending USB SetReport messages with a payload
 * of just 1 byte and report type output. It's type 0x21 (USB_REQ_DIR_OUT |
 * USB_REQ_TYP_CLASS | USB_REQ_REC_IFACE) Request code for SetReport is 0x09,
 * report type is 0x02 (HID_REPORT_TYPE_OUTPUT). We get a set_report callback
 * from TinyUSB device HID and then figure out what to do with the LEDs.
 */
void tud_hid_set_report_cb(uint8_t instance,
                           uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer,
                           uint16_t bufsize) {

    /* In config mode HID instance 3 is the separate helper channel after MSC;
       in normal mode instances 2 and 3 are the two helper channels. */
    if (report_id == 0 && (global_state.config_mode_active
                              ? instance == ITF_NUM_HID_CHANNEL_1
                              : (instance == ITF_NUM_HID_VENDOR || instance == ITF_NUM_HID_CHANNEL_1))) {
        if (report_type == HID_REPORT_TYPE_OUTPUT)
            channel_receive_report(global_state.config_mode_active ? 0 : instance - ITF_NUM_HID_VENDOR,
                                   buffer, bufsize);
        return;
    }

    /* We received a report on the config report ID */
    if (instance == ITF_NUM_HID_VENDOR && report_id == REPORT_ID_VENDOR) {
        /* Security - only if config mode is enabled are we allowed to do anything. While the report_id
           isn't even advertised when not in config mode, security must always be explicit and never assume */
        if (!global_state.config_mode_active)
            return;

        /* We insist on a fixed size packet. No overflows. */
        if (bufsize != RAW_PACKET_LENGTH)
            return;

        uart_packet_t *packet = (uart_packet_t *) (buffer + START_LENGTH);

        /* Only a certain packet types are accepted */
        if (!validate_packet(packet))
            return;

        process_packet(packet, &global_state);
    }

    /* Only other set report we care about is LED state change, and that's exactly 1 byte long */
    if (instance != ITF_NUM_HID || bufsize != 1 || report_type != HID_REPORT_TYPE_OUTPUT
        || report_id != (tud_hid_n_get_protocol(instance) == HID_PROTOCOL_BOOT
                         ? 0 : REPORT_ID_KEYBOARD))
        return;

    uint8_t leds = buffer[0];

    /* If we are using caps lock LED to indicate the chosen output, that has priority */
    if (global_state.config.kbd_led_as_indicator) {
        leds = leds & 0xFD; /* 1111 1101 (Clear Caps Lock bit) */

        if (global_state.active_output)
            leds |= KEYBOARD_LED_CAPSLOCK;
    }

    global_state.keyboard_leds_desired[BOARD_ROLE] = leds;

    /* If the board has a keyboard connected directly, restore those leds. */
    if (global_state.keyboard_connected && CURRENT_BOARD_IS_ACTIVE_OUTPUT)
        restore_leds(&global_state);

    /* Always send to the other one, so it is aware of the change */
    (void)send_value(leds, KBD_SET_REPORT_MSG);
}
