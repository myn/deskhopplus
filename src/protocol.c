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
#include "main.h"

const field_map_t api_field_map[] = {
/* Index, Rdonly, Type, Len, Offset in struct */
    { 0,  true,  UINT8,  1, offsetof(device_t, active_output) },
    { 1,  true,  INT16,  2, offsetof(device_t, pointer_x) },
    { 2,  true,  INT16,  2, offsetof(device_t, pointer_y) },
    { 3,  true,  INT16,  2, offsetof(device_t, mouse_buttons) },

    /* Output A */
    { 10, false, UINT32, 4, offsetof(device_t, config.output[0].number) },
    { 11, false, UINT32, 4, offsetof(device_t, config.output[0].screen_count) },
    { 12, false, INT32,  4, offsetof(device_t, config.output[0].speed_x) },
    { 13, false, INT32,  4, offsetof(device_t, config.output[0].speed_y) },
    { 14, false, INT32,  4, offsetof(device_t, config.output[0].border.top) },
    { 15, false, INT32,  4, offsetof(device_t, config.output[0].border.bottom) },
    { 16, false, UINT8,  1, offsetof(device_t, config.output[0].os) },
    { 17, false, UINT8,  1, offsetof(device_t, config.output[0].pos) },
    { 18, false, UINT8,  1, offsetof(device_t, config.output[0].mouse_park_pos) },
    { 19, false, UINT8,  1, offsetof(device_t, config.output[0].screensaver.mode) },
    { 20, false, UINT8,  1, offsetof(device_t, config.output[0].screensaver.only_if_inactive) },

    /* Until we increase the payload size from 8 bytes, clamp to avoid exceeding the field size */
    { 21, false, UINT64, 7, offsetof(device_t, config.output[0].screensaver.idle_time_us) },
    { 22, false, UINT64, 7, offsetof(device_t, config.output[0].screensaver.max_time_us) },

    /* Output B */
    { 40, false, UINT32, 4, offsetof(device_t, config.output[1].number) },
    { 41, false, UINT32, 4, offsetof(device_t, config.output[1].screen_count) },
    { 42, false, INT32,  4, offsetof(device_t, config.output[1].speed_x) },
    { 43, false, INT32,  4, offsetof(device_t, config.output[1].speed_y) },
    { 44, false, INT32,  4, offsetof(device_t, config.output[1].border.top) },
    { 45, false, INT32,  4, offsetof(device_t, config.output[1].border.bottom) },
    { 46, false, UINT8,  1, offsetof(device_t, config.output[1].os) },
    { 47, false, UINT8,  1, offsetof(device_t, config.output[1].pos) },
    { 48, false, UINT8,  1, offsetof(device_t, config.output[1].mouse_park_pos) },
    { 49, false, UINT8,  1, offsetof(device_t, config.output[1].screensaver.mode) },
    { 50, false, UINT8,  1, offsetof(device_t, config.output[1].screensaver.only_if_inactive) },
    { 51, false, UINT64, 7, offsetof(device_t, config.output[1].screensaver.idle_time_us) },
    { 52, false, UINT64, 7, offsetof(device_t, config.output[1].screensaver.max_time_us) },

    /* Common config */
    { 70, false, UINT32, 4, offsetof(device_t, config.version) },
    { 71, false, UINT8,  1, offsetof(device_t, config.force_mouse_boot_mode) },
    { 72, false, UINT8,  1, offsetof(device_t, config.force_kbd_boot_protocol) },
    { 73, false, UINT8,  1, offsetof(device_t, config.kbd_led_as_indicator) },
    { 74, false, UINT8,  1, offsetof(device_t, config.hotkey_toggle) },
    { 75, false, UINT8,  1, offsetof(device_t, config.enable_acceleration) },
    { 76, false, UINT8,  1, offsetof(device_t, config.enforce_ports) },
    { 77, false, UINT16, 2, offsetof(device_t, config.jump_threshold) },

    /* Firmware */
    { 78, true,  UINT16, 2, offsetof(device_t, _running_fw.version) },
    { 79, true,  UINT32, 4, offsetof(device_t, _running_fw.checksum) },

    /* What the *other* board reports running, or 0 when none has been heard
       from (#89). Read-only like the local version above it.

       The checksum is what makes propagation checkable at all since #91: two
       boards can hold different images at the same version, so 84 alone can
       report a match that is not one. 85 against 79 is the comparison that
       does not lie. */
    { 84, true,  UINT16, 2, offsetof(device_t, peer_fw.version) },
    { 85, true,  UINT32, 4, offsetof(device_t, peer_fw.checksum) },

    { 80, true,  UINT8,  1, offsetof(device_t, keyboard_connected) },
    { 81, true,  UINT8,  1, offsetof(device_t, switch_lock) },
    { 82, true,  UINT8,  1, offsetof(device_t, relative_mouse) },
    { 83, true,  UINT8,  1, offsetof(device_t, dev_build) },

    /* Which helper this board is paired with (#114), as the key id its hellos
       carry — SHA-256 of its public key, first 8 bytes. Until now the answer
       lived nowhere a user could reach.

       Read-only, and that is what makes it safe to expose: the config page
       writes a changed field to *both* boards, so a writable registration
       field would let one board's pairing evict the other computer's helper.
       handle_api_msgs refuses a SET on a read-only entry.

       Two halves because one field carries at most seven bytes and a key id is
       eight; the page joins them. The shared secret beside it stays out of the
       map entirely — it is the one part of the registration that is a secret,
       and it never leaves the board. */
    { 86, true,  UINT32, 4, offsetof(device_t, config.channel_helper_key_id) },
    { 87, true,  UINT32, 4, offsetof(device_t, config.channel_helper_key_id) + 4 },
    { 88, true,  UINT8,  1, offsetof(device_t, config.channel_paired) },

    /* Clipboard sharing, one toggle per direction (#52). Writable, and written
       to *both* boards by the config page — which is what the direction names
       depend on: "A to B" means the same thing on either board only while both
       hold the same pair of values.

       Stored as blocks so that zero means allowed; config_layout.h says why.
       The board turns them into what its own helper acts on with
       dh_clip_policy_for, so neither the page nor a helper has to work out
       which end of a direction it is standing at. */
    { 89, false, UINT8,  1, offsetof(device_t, config.clip_block_a_to_b) },
    { 90, false, UINT8,  1, offsetof(device_t, config.clip_block_b_to_a) },

    /*
     * What this board has dropped on the helper channel, since boot.
     *
     * Every one of these was already counted — #43 made "counted rather than
     * silently dropped" the rule — but counted somewhere nobody can read is
     * silent in practice, which is the mistake #94 cost two days to. #52's
     * first hardware runs were spent guessing at exactly these four numbers
     * from a helper log that cannot see them.
     *
     * Read-only, and diagnostic rather than behavioural: a non-zero value
     * names which seam is losing frames, and each seam has a different remedy.
     *
     *   91  reports the USB callback could not hand to channel_task
     *   92  frames the peer board sent that core 0 had not drained
     *   93  frames the outbound queue to this helper refused (ADR-0005)
     *   94  packets the inter-board link refused
     */
    { 91, true,  UINT32, 4, offsetof(device_t, _channel_reports_dropped) },
    { 92, true,  UINT32, 4, offsetof(device_t, _channel_inbound_dropped) },
    { 93, true,  UINT32, 4, offsetof(device_t, _channel_outq_refused) },
    { 94, true,  UINT32, 4, offsetof(device_t, _channel_relay_dropped) },

    /*
     * The inter-board link's own three, and the reason this list grew twice.
     * The first four are all about *this* board's seams, so a frame lost
     * between the boards leaves every one of them reading zero on the board
     * that sent it — which is exactly what #52's size-dependent fault looked
     * like from the config page.
     *
     *   95  data packets that arrived with no start to attach to
     *   96  frames abandoned because packets went missing — the size-dependent
     *       one, since a longer payload is more packets and so more chances
     *   97  frames the relay's own outbound queue refused
     */
    { 95, true,  UINT32, 4, offsetof(device_t, _channel_relay_orphans) },
    { 96, true,  UINT32, 4, offsetof(device_t, _channel_relay_truncated) },
    { 97, true,  UINT32, 4, offsetof(device_t, _channel_relay_refused) },
};

/* Fields 86 and 87 cover the helper key id exactly. A wider key id would leave
   its tail unreadable, and the config page would show a truncated value as if
   it were the whole thing — silently, since nothing else here would change. */
_Static_assert(DH_KEY_ID_SIZE == 8,
               "fields 86 and 87 split the helper key id into two 4-byte halves");

const field_map_t* get_field_map_entry(uint32_t index) {
    for (unsigned int i = 0; i < ARRAY_SIZE(api_field_map); i++) {
        if (api_field_map[i].idx == index) {
            return &api_field_map[i];
        }
    }

    return NULL;
}


const field_map_t* get_field_map_index(uint32_t index) {
    /* Clamp potential overflows to last element. */
    if (index >= ARRAY_SIZE(api_field_map))
        index = ARRAY_SIZE(api_field_map) - 1;

    return &api_field_map[index];
}

size_t get_field_map_length(void) {
    return ARRAY_SIZE(api_field_map);
}

void _queue_packet(uint8_t *payload, device_t *state, uint8_t type, uint8_t len, uint8_t id, uint8_t inst) {
    hid_generic_pkt_t generic_packet = {
        .instance = inst,
        .report_id = id,
        .type = type,
        .len = len,
    };

    memcpy(generic_packet.data, payload, len);
    queue_try_add(&state->hid_queue_out, &generic_packet);
}

void queue_cfg_packet(uart_packet_t *packet, device_t *state) {
    /*
     * The config API and the helper channel share an interface slot, one per
     * mode. Outside config mode that slot is the channel, whose descriptor
     * declares no report ID — so sending this would put REPORT_ID_VENDOR on
     * the wire as the first byte of the helper's frame stream, which reads it
     * as an unknown message type and drops the connection. Reachable without
     * config mode on this board: a config-mode peer can proxy an API read
     * over the UART (handle_api_msgs).
     */
    if (!state->config_mode_active)
        return;

    uint8_t raw_packet[RAW_PACKET_LENGTH];
    write_raw_packet(raw_packet, packet);
    _queue_packet(raw_packet, state, 0, RAW_PACKET_LENGTH, REPORT_ID_VENDOR, ITF_NUM_HID_VENDOR);
}

void queue_cc_packet(uint8_t *payload, device_t *state) {
    _queue_packet(payload, state, 1, CONSUMER_CONTROL_LENGTH, REPORT_ID_CONSUMER, ITF_NUM_HID);
}

void queue_system_packet(uint8_t *payload, device_t *state) {
    _queue_packet(payload, state, 2, SYSTEM_CONTROL_LENGTH, REPORT_ID_SYSTEM, ITF_NUM_HID);
}
