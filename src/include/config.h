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
bool queue_cfg_packet(uart_packet_t *, device_t *);
void reset_config_timer(device_t *);
void save_config(device_t *);
bool validate_packet(uart_packet_t *);
void wipe_config(void);

/*==============================================================================
 *  The board's identity (#111)
 *  Its own flash sector, beside the configuration and part of neither it nor
 *  the running image — so a config wipe, a firmware update and a peer
 *  propagation all leave it alone. See src/include/flash_layout.h.
 *==============================================================================*/

/* Read the stored private key back. False when the sector holds nothing this
   firmware can use, which is a board that has never booted, or one whose
   record was disturbed — either way the caller draws a fresh key. */
bool load_identity(uint8_t private_key[DH_P256_PRIVATE_SIZE]);

/* Write one. Erases and rewrites the identity sector, which is why it is
   called once, at boot, and never again for the life of the board. */
void save_identity(device_t *, const uint8_t private_key[DH_P256_PRIVATE_SIZE]);
