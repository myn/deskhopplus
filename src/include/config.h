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

#include <stdint.h>
#include "structs.h"
#include "misc.h"
#include "screen.h"

/* CURRENT_CONFIG_VERSION and CONFIG_MAGIC_HEADER live in config_layout.h,
   alongside the layout they describe, and the version history moved with them
   (#74). Reachable from here via structs.h. */

/*==============================================================================
 *  Configuration Data
 *  Structures and variables related to device configuration.
 *==============================================================================*/

extern const config_t default_config;

/*==============================================================================
 *  Configuration API
 *  Functions and data structures for accessing and modifying configuration.
 *==============================================================================*/

extern const field_map_t api_field_map[];
const field_map_t* get_field_map_entry(uint32_t);
const field_map_t* get_field_map_index(uint32_t);
size_t             get_field_map_length(void);

/*==============================================================================
 *  Configuration Management and Packet Processing
 *  Functions for loading, saving, wiping, and resetting device configuration.
 *==============================================================================*/

void load_config(device_t *);
void queue_cfg_packet(uart_packet_t *, device_t *);
void reset_config_timer(device_t *);
void save_config(device_t *);
bool validate_packet(uart_packet_t *);
void wipe_config(void);
