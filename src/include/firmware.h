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

 /*==============================================================================
  *  Firmware Update Functions
  *  Functions for managing firmware updates, CRC calculation, and related tasks.
  *==============================================================================*/

 uint32_t calculate_firmware_crc32(void);
 void     reboot(void);
 void     write_flash_page(uint32_t, uint8_t *);

 /* Give up on an upgrade being received (#90). Returns if nothing had been
    written over the running image yet; otherwise recovers to ROM and doesn't. */
 void     abandon_firmware_upgrade(device_t *);

 /* Wipe the stage 2 bootloader and reboot into the ROM bootloader. The running
    image cannot be trusted to boot, so this never returns. */
 void     recover_to_rom(void);

 /*==============================================================================
  *  UART Packet Fetching
  *  Functions to handle incoming UART packets, especially for firmware updates.
  *==============================================================================*/
 void     fetch_packet(device_t *);
 uint32_t get_ptr_delta(uint32_t, device_t *);
 bool     is_start_of_packet(device_t *);
 void     request_byte(device_t *, uint32_t);

 /*==============================================================================
  *  Button Interaction
  *  Functions interacting with the button, e.g. checking if pressed.
  *==============================================================================*/

 bool is_bootsel_pressed(void);
