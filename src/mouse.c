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

/* If we are active output, queue packet to mouse queue, else send them through UART */
void output_mouse_report(mouse_report_t *report, device_t *state) {
    if (CURRENT_BOARD_IS_ACTIVE_OUTPUT) {
        queue_mouse_report(report, state);
        state->last_activity[BOARD_ROLE] = time_us_64();
    } else {
        (void)queue_packet((uint8_t *)report, MOUSE_REPORT_MSG, MOUSE_REPORT_LENGTH);
    }
}

static inline bool extract_value(bool uses_id, int32_t *dst, report_val_t *src, uint8_t *raw_report, int len) {
    /* If HID Report ID is used, the report is prefixed by the report ID so we have to move by 1 byte */
    if (uses_id && (*raw_report++ != src->report_id))
        return false;

    *dst = get_report_value(raw_report, len, src);
    return true;
}

void extract_report_values(uint8_t *raw_report, int len, device_t *state, mouse_values_t *values, hid_interface_t *iface) {
    /* Interpret values depending on the current protocol used. */
    if (iface->protocol == HID_PROTOCOL_BOOT) {
        hid_mouse_report_t *mouse_report = (hid_mouse_report_t *)raw_report;

        values->move_x  = mouse_report->x;
        values->move_y  = mouse_report->y;
        values->wheel   = mouse_report->wheel;
        values->pan     = mouse_report->pan;
        values->buttons = mouse_report->buttons;
        return;
    }
    mouse_t *mouse = &iface->mouse;
    bool uses_id = iface->uses_report_id;

    extract_value(uses_id, &values->move_x, &mouse->move_x, raw_report, len);
    extract_value(uses_id, &values->move_y, &mouse->move_y, raw_report, len);
    extract_value(uses_id, &values->wheel, &mouse->wheel, raw_report, len);
    extract_value(uses_id, &values->pan, &mouse->pan, raw_report, len);

    if (!extract_value(uses_id, &values->buttons, &mouse->buttons, raw_report, len)) {
        values->buttons = state->mouse_buttons;
    }
}

void process_mouse_report(uint8_t *raw_report, int len, uint8_t itf, hid_interface_t *iface) {
    mouse_values_t values = {0};
    device_t *state = &global_state;

    /* Interpret the mouse HID report, extract and save values we need. */
    extract_report_values(raw_report, len, state, &values, iface);

    process_mouse_values(state, &values);
}

/* ==================================================== *
 * Mouse Queue Section
 * ==================================================== */

void process_mouse_queue_task(device_t *state) {
    mouse_report_t report = {0};

    /* We need to be connected to the host to send messages */
    if (!state->tud_connected)
        return;

    /* Peek first, if there is anything there... */
    if (!queue_try_peek(&state->mouse_queue, &report))
        return;

    /* If we are suspended, let's wake the host up */
    if (tud_suspended())
        tud_remote_wakeup();

    /* If it's not ready, we'll try on the next pass */
    if (!tud_hid_n_ready(ITF_NUM_HID))
        return;

    /* Try sending it to the host, if it's successful */
    bool succeeded
        = tud_mouse_report(report.mode, report.buttons, report.x, report.y, report.wheel, report.pan);

    /* ... then we can remove it from the queue */
    if (succeeded)
        queue_try_remove(&state->mouse_queue, &report);
}

void queue_mouse_report(mouse_report_t *report, device_t *state) {
    /* It wouldn't be fun to queue up a bunch of messages and then dump them all on host */
    if (!state->tud_connected)
        return;

    queue_try_add(&state->mouse_queue, report);
}
