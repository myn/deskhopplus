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
#pragma once

#include "structs.h"
#include "hid_parser.h"

/*==============================================================================
 *  Data Extraction
 *==============================================================================*/
void      extract_data(hid_interface_t *, report_val_t *);
int32_t   get_report_value(uint8_t *, int, report_val_t *);
void      parse_report_descriptor(hid_interface_t *, uint8_t const *, int);

/*==============================================================================
 *  Mouse Report Handling
 *==============================================================================*/
void process_mouse_report(uint8_t *, int, uint8_t, hid_interface_t *);
enum screen_pos_e update_mouse_position(device_t *, mouse_values_t *);
void do_screen_switch(device_t *, int);
void mouse_crossing_task(device_t *, uint32_t now_us);
void mouse_crossing_query_unavailable(device_t *, uint8_t output, uint8_t query_id);
void switch_to_another_pc(device_t *, output_t *, int, int);
void switch_virtual_desktop(device_t *, output_t *, int, int);
void queue_mouse_report(mouse_report_t *, device_t *);
bool tud_mouse_report(uint8_t mode, uint8_t buttons, int16_t x, int16_t y, int8_t wheel, int8_t pan);
void output_mouse_report(mouse_report_t *, device_t *);
