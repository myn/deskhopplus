/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Derek Reynolds */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "tusb.h"
#include "constants.h"
#include "usb_descriptors.h"
#include "core/dh_keyboard_output.h"

#define PICO_UNIQUE_BOARD_ID_SIZE_BYTES 8
void pico_get_unique_board_id_string(char *buffer, unsigned size);

typedef struct TU_ATTR_PACKED {
    uint8_t buttons;
    int16_t x, y;
    int8_t wheel, pan;
    uint8_t mode;
} mouse_report_t;

typedef struct { int remaining; } queue_t;
typedef struct { uint8_t bytes[16]; } hid_generic_pkt_t;
typedef struct { int32_t speed_x, speed_y; dh_keymap_profile_t keymap; bool swap_ctrl_gui; } output_t;
bool queue_try_remove(queue_t *queue, void *item);
void tud_mouse_report_reset(int16_t x, int16_t y);

typedef struct {
    bool config_mode_active;
    uint8_t board_role, active_output;
    uint8_t keyboard_leds_desired[2];
    bool keyboard_connected;
    bool mouse_zoom;
    bool boot_mouse_mode[2];
    int16_t pointer_x, pointer_y;
    queue_t kbd_queue, mouse_queue, hid_queue_out;
    struct {
        bool kbd_led_as_indicator;
        output_t output[2];
    } config;
} device_t;
extern device_t global_state;

#define BOARD_ROLE (global_state.board_role)
#define START_LENGTH 2
#define RAW_PACKET_LENGTH 12
typedef struct { uint8_t bytes[10]; } uart_packet_t;
enum packet_type_e { KBD_SET_REPORT_MSG = 6, BOOT_MOUSE_MODE_MSG = 35 };

void combine_kbd_states(device_t *state, hid_keyboard_report_t *report);
void channel_receive_report(uint8_t index, const uint8_t *buffer, uint16_t bufsize);
bool validate_packet(uart_packet_t *packet);
void process_packet(uart_packet_t *packet, device_t *state);
void restore_leds(device_t *state);
bool send_value(uint8_t value, enum packet_type_e type);
