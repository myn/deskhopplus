/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Derek Reynolds */

#include <stdio.h>
#include <string.h>
#include "main.h"
#include "user_config.h"

extern uint8_t const desc_configuration[];
extern uint8_t const desc_configuration_config[];
extern uint8_t const *tud_descriptor_configuration_cb(uint8_t index);
extern bool tud_keyboard_report(const hid_keyboard_report_t *report);
extern bool tud_mouse_report(uint8_t mode, uint8_t buttons, int16_t x, int16_t y, int8_t wheel, int8_t pan);
extern void tud_mouse_report_reset(int16_t x, int16_t y);

device_t global_state;
static uint8_t protocol[2] = {HID_PROTOCOL_REPORT, HID_PROTOCOL_REPORT};
static uint8_t last_instance, last_id, payload[32];
static uint16_t payload_len;
static hid_keyboard_report_t held_keys = {.modifier = 2, .reserved = 0x42, .keycode = {4}};
static uint8_t sent_value, received_channel;
static enum packet_type_e sent_type;
static int channel_calls;
static int failures;

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

void pico_get_unique_board_id_string(char *buffer, unsigned size) {
    if (size) buffer[0] = 0;
}

uint8_t tud_hid_n_get_protocol(uint8_t instance) { return protocol[instance]; }
bool tud_hid_n_report(uint8_t instance, uint8_t report_id, const void *report, uint16_t len) {
    last_instance = instance;
    last_id = report_id;
    payload_len = len;
    memcpy(payload, report, len);
    return true;
}
bool tud_hid_n_keyboard_report(uint8_t instance, uint8_t id, uint8_t modifier, uint8_t keycode[6]) {
    hid_keyboard_report_t report = {.modifier = modifier};
    memcpy(report.keycode, keycode, 6);
    return tud_hid_n_report(instance, id, &report, sizeof(report));
}

void combine_kbd_states(device_t *state, hid_keyboard_report_t *report) {
    (void)state;
    *report = held_keys;
}
void dh_keyboard_output_prepare(const uint8_t *input, dh_keyboard_provenance provenance,
                                const dh_keymap_profile_t *profile, bool swap, uint8_t *output) {
    (void)provenance; (void)profile; (void)swap;
    memcpy(output, input, 8);
}
void channel_receive_report(uint8_t index, const uint8_t *buffer, uint16_t size) {
    if (size == 1) received_channel = index;
    (void)buffer;
    channel_calls++;
}
bool validate_packet(uart_packet_t *packet) { (void)packet; return false; }
void process_packet(uart_packet_t *packet, device_t *state) { (void)packet; (void)state; }
void restore_leds(device_t *state) { (void)state; }
static hid_keyboard_report_t queued_keys;
static int queued_key_reports;
void queue_kbd_report(hid_keyboard_report_t *report, device_t *state) {
    (void)state;
    queued_keys = *report;
    queued_key_reports++;
}
static hid_keyboard_report_t local_keys, sent_peer_keys;
static int peer_key_reports;
static dh_keyboard_provenance sent_peer_provenance;
void combine_local_kbd_states(device_t *state, hid_keyboard_report_t *report) {
    (void)state;
    *report = local_keys;
}
bool queue_remote_keyboard_report(const hid_keyboard_report_t *report,
                                  dh_keyboard_provenance provenance) {
    sent_peer_provenance = provenance;
    sent_peer_keys = *report;
    peer_key_reports++;
    return true;
}
static mouse_report_t queued_mouse;
static int queued_mouse_reports;
void queue_mouse_report(mouse_report_t *report, device_t *state) {
    (void)state;
    queued_mouse = *report;
    queued_mouse_reports++;
}
bool send_value(uint8_t value, enum packet_type_e type) {
    sent_type = type;
    sent_value = value;
    return true;
}
bool queue_try_remove(queue_t *queue, void *item) {
    (void)item;
    if (!queue->remaining) return false;
    queue->remaining--;
    return true;
}

static void check_interfaces(const uint8_t *desc, size_t length, bool config) {
    bool seen[5] = {0};
    for (size_t pos = 0; pos < length; pos += desc[pos]) {
        CHECK(desc[pos] >= 2 && pos + desc[pos] <= length);
        if (desc[pos] < 2 || pos + desc[pos] > length) return;
        if (desc[pos + 1] != TUSB_DESC_INTERFACE) continue;
        uint8_t itf = desc[pos + 2];
        CHECK(itf < 5);
        if (itf >= 5) continue;
        seen[itf] = true;
        if (itf == ITF_NUM_HID || itf == ITF_NUM_HID_REL_M) {
            CHECK(desc[pos + 6] == HID_SUBCLASS_BOOT);
            CHECK(desc[pos + 7] == (itf == ITF_NUM_HID ? HID_ITF_PROTOCOL_KEYBOARD : HID_ITF_PROTOCOL_MOUSE));
        } else if (desc[pos + 5] == TUSB_CLASS_HID) {
            CHECK(desc[pos + 6] == 0 && desc[pos + 7] == 0);
        }
        if ((!config && (itf == 2 || itf == 3)) || (config && itf == 4))
            CHECK(desc[pos + 5] == TUSB_CLASS_HID && desc[pos + 4] == 2);
    }
    CHECK(seen[0] && seen[1] && seen[2] && seen[3]);
    CHECK(seen[4] == config);
}

int main(void) {
    global_state.config.output[0].speed_x = MOUSE_SPEED_A_FACTOR_X;
    global_state.config.output[0].speed_y = MOUSE_SPEED_A_FACTOR_Y;
    check_interfaces(desc_configuration, TUD_CONFIG_DESC_LEN + 2 * TUD_HID_DESC_LEN + 2 * TUD_HID_INOUT_DESC_LEN, false);
    check_interfaces(desc_configuration_config, TUD_CONFIG_DESC_LEN + 3 * TUD_HID_DESC_LEN + TUD_MSC_DESC_LEN + TUD_HID_INOUT_DESC_LEN, true);
    CHECK(tud_descriptor_configuration_cb(0) == desc_configuration);
    global_state.config_mode_active = true;
    CHECK(tud_descriptor_configuration_cb(0) == desc_configuration_config);
    global_state.config_mode_active = false;

    hid_keyboard_report_t keys = {.modifier = 0x02, .reserved = 0x42, .keycode = {4, 5}};
    CHECK(tud_keyboard_report(&keys));
    CHECK(last_instance == 0 && last_id == REPORT_ID_KEYBOARD && payload_len == 8);
    protocol[0] = HID_PROTOCOL_BOOT;
    CHECK(tud_keyboard_report(&keys));
    CHECK(last_instance == 0 && last_id == 0 && payload_len == 8);
    CHECK(payload[0] == keys.modifier && payload[1] == 0 && memcmp(payload + 2, keys.keycode, 6) == 0);

    /* A report armed before SET_PROTOCOL still goes out in the old format: the
       boot host reads report ID 01 as a held Left Ctrl (#67). The switch must
       queue a fresh report of the output's current keys to replace it. */
    tud_hid_set_protocol_cb(0, HID_PROTOCOL_BOOT);
    CHECK(queued_key_reports == 1 && queued_keys.modifier == held_keys.modifier
          && memcmp(queued_keys.keycode, held_keys.keycode, 6) == 0);
    global_state.active_output = 1;
    tud_hid_set_protocol_cb(0, HID_PROTOCOL_BOOT);
    CHECK(queued_key_reports == 2 && memcmp(&queued_keys, (uint8_t[8]){0}, 8) == 0);
    global_state.active_output = 0;
    protocol[0] = HID_PROTOCOL_REPORT;
    tud_hid_set_protocol_cb(0, HID_PROTOCOL_REPORT);
    CHECK(queued_key_reports == 3 && queued_keys.modifier == held_keys.modifier);
    protocol[0] = HID_PROTOCOL_BOOT;

    CHECK(tud_mouse_report(RELATIVE, 3, 300, -300, 1, 2));
    CHECK(last_instance == 1 && last_id == REPORT_ID_RELMOUSE && payload_len == sizeof(mouse_report_t));
    protocol[1] = HID_PROTOCOL_BOOT;
    CHECK(tud_mouse_report(RELATIVE, 3, 300, -300, 1, 2));
    CHECK(last_instance == 1 && last_id == 0 && payload_len == 3);
    CHECK(payload[0] == 3 && payload[1] == 127 && payload[2] == 128);
    CHECK(tud_mouse_report(RELATIVE, 1, -5, 6, 0, 0));
    CHECK(payload[0] == 1 && payload[1] == 251 && payload[2] == 6);
    global_state.pointer_x = 10;
    global_state.pointer_y = 20;
    tud_hid_set_protocol_cb(1, HID_PROTOCOL_BOOT);
    CHECK(global_state.boot_mouse_mode[0] && sent_type == BOOT_MOUSE_MODE_MSG && sent_value == 1);
    /* Same stale-report hazard as the keyboard: report ID 05 read as buttons
       would hold left and middle. A still report replaces it. */
    CHECK(queued_mouse_reports == 1 && queued_mouse.mode == RELATIVE
          && queued_mouse.buttons == 0 && queued_mouse.x == 0 && queued_mouse.y == 0);
    CHECK(tud_mouse_report(ABSOLUTE, 1, 100, 50, 0, 0));
    CHECK(last_instance == 1 && last_id == 0 && payload_len == 3);
    CHECK(payload[0] == 1 && payload[1] == 5 && payload[2] == 1);
    CHECK(tud_mouse_report(ABSOLUTE, 2, 300, -300, 0, 0));
    CHECK(payload[0] == 2 && payload[1] == 12 && payload[2] == 244);
    /* One physical count becomes these absolute-coordinate steps at default speed.
       A boot host must receive one relative count, not the screen-space step. */
    tud_mouse_report_reset(100, 50);
    CHECK(tud_mouse_report(ABSOLUTE, 0,
                           100 + MOUSE_SPEED_A_FACTOR_X,
                           50 + MOUSE_SPEED_A_FACTOR_Y, 0, 0));
    CHECK(payload[1] == 1 && payload[2] == 1);
    global_state.board_role = 1;
    global_state.config.output[1].speed_x = 12;
    global_state.config.output[1].speed_y = 20;
    tud_mouse_report_reset(100, 50);
    CHECK(tud_mouse_report(ABSOLUTE, 0, 112, 70, 0, 0));
    CHECK(payload[1] == 1 && payload[2] == 1);
    global_state.mouse_zoom = true;
    tud_mouse_report_reset(100, 50);
    CHECK(tud_mouse_report(ABSOLUTE, 0, 103, 55, 0, 0));
    CHECK(payload[1] == 1 && payload[2] == 1);
    global_state.mouse_zoom = false;
    global_state.board_role = 0;
    tud_mouse_report_reset(100, 0);
    CHECK(tud_mouse_report(BOOT_RELATIVE, 0, 0, -10, 0, 0));
    CHECK(last_instance == 1 && last_id == 0 && payload_len == 3);
    CHECK(payload[1] == 0 && payload[2] == 246);
    protocol[1] = HID_PROTOCOL_REPORT;
    tud_hid_set_protocol_cb(1, HID_PROTOCOL_REPORT);
    CHECK(!global_state.boot_mouse_mode[0] && sent_type == BOOT_MOUSE_MODE_MSG && sent_value == 0);
    CHECK(tud_mouse_report(RELATIVE, 1, -5, 6, 0, 0));
    CHECK(last_id == REPORT_ID_RELMOUSE && payload_len == sizeof(mouse_report_t));
    CHECK(tud_mouse_report(ABSOLUTE, 1, 10, 20, 0, 0));
    CHECK(last_instance == 1); /* The boot keyboard never receives an absolute-mouse report. */
    protocol[0] = HID_PROTOCOL_REPORT;
    CHECK(tud_mouse_report(ABSOLUTE, 1, 10, 20, 0, 0));
    CHECK(last_instance == 0 && last_id == REPORT_ID_MOUSE && payload_len == sizeof(mouse_report_t));

    uint8_t control[8] = {0xff};
    CHECK(tud_hid_get_report_cb(0, REPORT_ID_KEYBOARD, HID_REPORT_TYPE_INPUT, control, 8) == 8);
    CHECK(control[0] == held_keys.modifier && control[1] == 0 && memcmp(control + 2, held_keys.keycode, 6) == 0);
    global_state.active_output = 1;
    CHECK(tud_hid_get_report_cb(0, REPORT_ID_KEYBOARD, HID_REPORT_TYPE_INPUT, control, 8) == 8);
    CHECK(memcmp(control, (uint8_t[8]){0}, 8) == 0);
    global_state.active_output = 0;
    CHECK(tud_hid_get_report_cb(0, REPORT_ID_KEYBOARD, HID_REPORT_TYPE_INPUT, control, 7) == 0);
    CHECK(tud_hid_get_report_cb(2, REPORT_ID_KEYBOARD, HID_REPORT_TYPE_INPUT, control, 8) == 0);
    /* Like upstream, answer either keyboard control ID regardless of the
       current interrupt protocol, including before SET_PROTOCOL(boot). */
    CHECK(tud_hid_get_report_cb(0, 0, HID_REPORT_TYPE_INPUT, control, 8) == 8);
    CHECK(control[0] == held_keys.modifier && control[1] == 0 && memcmp(control + 2, held_keys.keycode, 6) == 0);
    protocol[0] = HID_PROTOCOL_BOOT;
    CHECK(tud_hid_get_report_cb(0, 0, HID_REPORT_TYPE_INPUT, control, 8) == 8);
    CHECK(control[0] == held_keys.modifier && control[1] == 0 && memcmp(control + 2, held_keys.keycode, 6) == 0);
    CHECK(tud_hid_get_report_cb(0, REPORT_ID_KEYBOARD, HID_REPORT_TYPE_INPUT, control, 8) == 8);
    CHECK(control[0] == held_keys.modifier && control[1] == 0 && memcmp(control + 2, held_keys.keycode, 6) == 0);
    CHECK(tud_hid_get_report_cb(0, REPORT_ID_MOUSE, HID_REPORT_TYPE_INPUT, control, 8) == 0);
    global_state.keyboard_leds_desired[0] = 2;
    CHECK(tud_hid_get_report_cb(0, 0, HID_REPORT_TYPE_OUTPUT, control, 1) == 1 && control[0] == 2);
    CHECK(tud_hid_get_report_cb(0, REPORT_ID_KEYBOARD, HID_REPORT_TYPE_OUTPUT, control, 1) == 1 && control[0] == 2);
    protocol[1] = HID_PROTOCOL_BOOT;
    CHECK(tud_hid_get_report_cb(1, 0, HID_REPORT_TYPE_INPUT, control, 3) == 3);
    CHECK(memcmp(control, (uint8_t[3]){0}, 3) == 0);

    uint8_t led = 1;
    tud_hid_set_report_cb(0, 0, HID_REPORT_TYPE_OUTPUT, &led, 1);
    CHECK(global_state.keyboard_leds_desired[0] == 1 && sent_value == 1);
    led = 7;
    tud_hid_set_report_cb(0, REPORT_ID_KEYBOARD, HID_REPORT_TYPE_OUTPUT, &led, 1);
    CHECK(global_state.keyboard_leds_desired[0] == 7 && sent_value == 7);
    led = 3;
    tud_hid_set_report_cb(1, 0, HID_REPORT_TYPE_OUTPUT, &led, 1);
    CHECK(global_state.keyboard_leds_desired[0] == 7);
    protocol[0] = HID_PROTOCOL_REPORT;
    led = 4;
    tud_hid_set_report_cb(0, REPORT_ID_KEYBOARD, HID_REPORT_TYPE_OUTPUT, &led, 1);
    CHECK(global_state.keyboard_leds_desired[0] == 4 && sent_value == 4);
    led = 5;
    tud_hid_set_report_cb(0, 0, HID_REPORT_TYPE_OUTPUT, &led, 1);
    CHECK(global_state.keyboard_leds_desired[0] == 5 && sent_value == 5);
    led = 6;
    tud_hid_set_report_cb(0, REPORT_ID_MOUSE, HID_REPORT_TYPE_OUTPUT, &led, 1);
    CHECK(global_state.keyboard_leds_desired[0] == 5);
    tud_hid_set_report_cb(2, 0, HID_REPORT_TYPE_OUTPUT, &led, 1);
    CHECK(channel_calls == 1 && received_channel == 0);
    global_state.config_mode_active = true;
    tud_hid_set_report_cb(3, 0, HID_REPORT_TYPE_OUTPUT, &led, 1);
    CHECK(channel_calls == 2 && received_channel == 0);

    /* Board B, Alt held on its keyboard before board A enumerates: keyboards
       report only changes, so the held Alt must be resent when board A says
       it connected, or the Mac never sees Option at power-on (#67). */
    global_state.config_mode_active = false;
    global_state.board_role = 1;
    global_state.active_output = 0;
    local_keys = (hid_keyboard_report_t){.modifier = KEYBOARD_MODIFIER_LEFTALT};
    uart_packet_t peer_state = {.type = BOOT_MOUSE_MODE_MSG, .data = {0}};
    handle_boot_mouse_mode_msg(&peer_state, &global_state);
    CHECK(peer_key_reports == 1 && sent_peer_keys.modifier == KEYBOARD_MODIFIER_LEFTALT
          && sent_peer_provenance == DH_KEYBOARD_PHYSICAL);
    /* Board B is the active output: its keys already go to its own computer. */
    global_state.active_output = 1;
    handle_boot_mouse_mode_msg(&peer_state, &global_state);
    CHECK(peer_key_reports == 1);
    global_state.board_role = 0;
    global_state.active_output = 0;

    global_state.kbd_queue.remaining = 3;
    global_state.mouse_queue.remaining = 4;
    global_state.hid_queue_out.remaining = 2;
    discard_queued_host_reports();
    CHECK(global_state.kbd_queue.remaining == 0 && global_state.mouse_queue.remaining == 0
          && global_state.hid_queue_out.remaining == 0);
    return failures ? 1 : 0;
}
