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

/* ================================================== *
 * ==============  Checksum Functions  ============== *
 * ================================================== */

uint8_t calc_checksum(const uint8_t *data, int length) {
    uint8_t checksum = 0;

    for (int i = 0; i < length; i++) {
        checksum ^= data[i];
    }

    return checksum;
}

bool verify_checksum(const uart_packet_t *packet) {
    uint8_t checksum = calc_checksum(packet->data, PACKET_DATA_LENGTH);
    return checksum == packet->checksum;
}

uint32_t crc32_iter(uint32_t crc, const uint8_t byte) {
    return crc32_lookup_table[(byte ^ crc) & 0xff] ^ (crc >> 8);
}

/* TODO - use DMA sniffer's built-in CRC32 */
uint32_t calc_crc32(const uint8_t *s, size_t n) {
    uint32_t crc = 0xffffffff;

    for(size_t i=0; i < n; i++) {
        crc = crc32_iter(crc, s[i]);
    }

    return ~crc;
}

uint32_t calculate_firmware_crc32(void) {
    return calc_crc32(ADDR_FW_RUNNING, STAGING_IMAGE_SIZE - FLASH_SECTOR_SIZE);
}

/* ================================================== *
 * Flash and config functions
 * ================================================== */

void wipe_config(void) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase((uint32_t)ADDR_CONFIG - XIP_BASE, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
}

void write_flash_page(uint32_t target_addr, uint8_t *buffer) {
    /* Start of sector == first 256-byte page in a 4096 byte block */
    bool is_sector_start = (target_addr & 0xf00) == 0;

    uint32_t ints = save_and_disable_interrupts();
    if (is_sector_start)
        flash_range_erase(target_addr, FLASH_SECTOR_SIZE);

    flash_range_program(target_addr, buffer, FLASH_PAGE_SIZE);
    restore_interrupts(ints);
}

/* Hand the board to the ROM bootloader, because the running image can no
   longer be trusted to boot: wiping the stage 2 bootloader is what makes the
   ROM take over on the next power-up rather than jumping into a broken image.

   Reached from three places that all mean the same thing — a checksum that did
   not match after a pull or a UF2 drop, and a transfer that could not be
   repaired by restarting it (#90). Failing loudly here costs the user a manual
   reflash; running an image that is part-old and part-new costs them a board
   that misbehaves in ways nothing explains. Does not return.

   Interrupts off around the erase for the same reason write_flash_page does
   it: an interrupt handler that fetches from XIP flash mid-erase faults the
   core, and here that would happen *after* the stage 2 sector is gone but
   before reset_usb_boot is reached — leaving a board with neither a bootable
   image nor an automatic way into ROM. */
void recover_to_rom(void) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase((uint32_t)ADDR_FW_RUNNING - XIP_BASE, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);

    reset_usb_boot(1 << PICO_DEFAULT_LED_PIN, 0);
}

/* Give up on an upgrade being received, so the board stops behaving as though
   one were in flight: the config-mode timeout can fire again, and the next
   heartbeat from a newer peer board is free to start the pull over (#90).

   Restarting is the repair, not merely a second chance. A pull runs from
   address 0 to the end of the image and writes every page on the way, so a
   run that completes overwrites whatever a previous run left half-written.
   While the peer board is still heartbeating, another attempt is therefore
   always the better move — see fw_upgrade_must_recover for why that stays
   true however many attempts it takes. Only a peer board that has gone makes
   the damage permanent, and that call does not return. */
void abandon_firmware_upgrade(device_t *state) {
    if (fw_upgrade_must_recover(&state->fw, state->peer_fw.version != PEER_FW_UNKNOWN))
        recover_to_rom();

    state->fw.upgrade_in_progress = false;
    state->fw.source              = FW_UPGRADE_SOURCE_NONE;
    state->fw.address             = 0;
}

void load_config(device_t *state) {
    const config_t *config   = ADDR_CONFIG;
    config_t *running_config = &state->config;

    /* Load the flash config first, including the checksum */
    memcpy(running_config, config, sizeof(config_t));

    /* Everything this function decides is in config_is_valid, which is pure
       and therefore testable (#74). All that is left here is the flash read
       and the fallback. */
    if (!config_is_valid(running_config) ||
        !dh_hotkey_table_is_valid(running_config->hotkeys, DH_HOTKEY_ACTION_COUNT))
        memcpy(running_config, &default_config, sizeof(config_t));

    prepare_hotkeys(running_config->hotkeys);
}

void save_config(device_t *state) {
    _Static_assert(CONFIG_FLASH_PAGE_SIZE == FLASH_PAGE_SIZE,
                   "config layout and Pico flash page sizes must agree");
    _Static_assert(CONFIG_FLASH_SECTOR_SIZE == FLASH_SECTOR_SIZE,
                   "config layout and Pico flash sector sizes must agree");
    uint8_t *raw_config = (uint8_t *)&state->config;

    /* The other half of the pair load_config uses, so the two cannot drift
       apart the way they did in #74. */
    config_seal(&state->config);

    /* Copy the config to buffer and pad the rest with zeros */
    memcpy(state->page_buffer, raw_config, sizeof(config_t));
    memset(state->page_buffer + sizeof(config_t), 0, CONFIG_FLASH_BYTES - sizeof(config_t));

    /* Write the new config to flash */
    for (size_t offset = 0; offset < CONFIG_FLASH_BYTES; offset += FLASH_PAGE_SIZE)
        write_flash_page((uint32_t)ADDR_CONFIG - XIP_BASE + offset, state->page_buffer + offset);
}

/* ================================================== *
 * The board's identity (#111)
 *
 * Its own sector, and everything about that placement is defensive: a config
 * wipe erases ADDR_CONFIG, a firmware update and a peer propagation write the
 * running image from address 0, and neither range reaches here
 * (src/include/flash_layout.h asserts it, tests/flash_layout_test.c gates it).
 * An identity inside the image would give both boards the same identity, which
 * is precisely what the propagation path in #91 would produce.
 * ================================================== */

bool load_identity(uint8_t private_key[DH_P256_PRIVATE_SIZE]) {
    identity_t stored;

    /* Copied out before it is judged, exactly as load_config does: the flash
       is memory-mapped and read-only, and the checksum has to be computed over
       a struct this firmware laid out rather than over whatever the sector
       happens to alias. */
    memcpy(&stored, ADDR_IDENTITY, sizeof stored);

    if (!identity_is_valid(&stored))
        return false;

    memcpy(private_key, stored.private_key, DH_P256_PRIVATE_SIZE);
    return true;
}

void save_identity(device_t *state, const uint8_t private_key[DH_P256_PRIVATE_SIZE]) {
    identity_t fresh = {
        .magic_header = IDENTITY_MAGIC_HEADER,
        .version = CURRENT_IDENTITY_VERSION,
    };
    memcpy(fresh.private_key, private_key, DH_P256_PRIVATE_SIZE);
    identity_seal(&fresh);

    memcpy(state->page_buffer, &fresh, sizeof fresh);
    memset(state->page_buffer + sizeof fresh, 0, FLASH_PAGE_SIZE - sizeof fresh);

    write_flash_page((uint32_t)ADDR_IDENTITY - XIP_BASE, state->page_buffer);
}

void reset_config_timer(device_t *state) {
    /* Once this is reached, we leave the config mode */
    state->config_mode_timer = time_us_64() + CONFIG_MODE_TIMEOUT;
}

void _configure_flash_cs(enum gpio_override gpo, uint pin_index) {
  hw_write_masked(&ioqspi_hw->io[pin_index].ctrl,
                  gpo << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                  IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
}

bool is_bootsel_pressed(void) {
  const uint CS_PIN_INDEX = 1;
  uint32_t flags = save_and_disable_interrupts();

  /* Set chip select to high impedance */
  _configure_flash_cs(GPIO_OVERRIDE_LOW, CS_PIN_INDEX);
  sleep_us(20);

  /* Button pressed pulls pin DOWN, so invert */
  bool button_pressed = !(sio_hw->gpio_hi_in & (1u << CS_PIN_INDEX));

  /* Restore chip select state */
  _configure_flash_cs(GPIO_OVERRIDE_NORMAL, CS_PIN_INDEX);
  restore_interrupts(flags);

  return button_pressed;
}

void request_byte(device_t *state, uint32_t address) {
    uart_packet_t packet = {
        .data32[0] = address,
        .type = REQUEST_BYTE_MSG,
    };

    /* Only treat the byte as outstanding if the request actually went out.
       uart_tx_queue is shared with channel_task on core0, so it can fill
       between firmware_upgrade_task's queue_is_full check and this enqueue —
       and clearing byte_done on a request that was dropped is a pull that
       never advances again. That is #90's hang at its finest grain, and it is
       the same discipline channel_pump_relay already states: a refused enqueue
       leaves the packet owed rather than lost.

       Leaving byte_done set means the task simply asks again on its next pass,
       at 4 kHz, long before the 30 s stall window is reached.

       The timestamp is set either way. On success it starts the clock on an
       outstanding word, which is what fw_upgrade_request_lost measures. On a
       refusal it costs one re-request interval before the next attempt, which
       is the point: retrying a full queue at 4 kHz would drown #43's drop
       statistics in attempts that were never going to land. */
    state->fw.requested_at_us = time_us_32();

    if (queue_uart_packet(&packet, state))
        state->fw.byte_done = false;
}

void reboot(void) {
    *((volatile uint32_t*)(PPB_BASE + 0x0ED0C)) = 0x5FA0004;
}

bool is_start_of_packet(device_t *state) {
    return (uart_rxbuf[state->dma_ptr] == START1 && uart_rxbuf[NEXT_RING_IDX(state->dma_ptr)] == START2);
}

uint32_t get_ptr_delta(uint32_t current_pointer, device_t *state) {
    uint32_t delta;

    if (current_pointer >= state->dma_ptr)
        delta = current_pointer - state->dma_ptr;
    else
        delta = DMA_RX_BUFFER_SIZE - state->dma_ptr + current_pointer;

    /* Clamp to 12 bits since it can never be bigger */
    delta = delta & 0x3FF;

    return delta;
}

void fetch_packet(device_t *state) {
    uint8_t *dst = (uint8_t *)&state->in_packet;

    for (int i = 0; i < RAW_PACKET_LENGTH; i++) {
        /* Skip the header preamble */
        if (i >= START_LENGTH)
            dst[i - START_LENGTH] = uart_rxbuf[state->dma_ptr];

        state->dma_ptr = NEXT_RING_IDX(state->dma_ptr);
    }
}

/* Validating any input is mandatory. Only packets of these type are allowed
   to be sent to the device over configuration endpoint. */
bool validate_packet(uart_packet_t *packet) {
    const enum packet_type_e ALLOWED_PACKETS[] = {
        FLASH_LED_MSG,
        GET_VAL_MSG,
        GET_ALL_VALS_MSG,
        SET_VAL_MSG,
        WIPE_CONFIG_MSG,
        SAVE_CONFIG_MSG,
        REBOOT_MSG,
        PROXY_PACKET_MSG,
        GET_CURSOR_TRACE_MSG,
    };
    uint8_t packet_type = packet->type;

    /* Proxied packets are encapsulated in the data field, but same rules apply */
    if (packet->type == PROXY_PACKET_MSG)
        packet_type = packet->data[0];

    for (int i = 0; i < ARRAY_SIZE(ALLOWED_PACKETS); i++) {
        if (ALLOWED_PACKETS[i] == packet_type)
            return true;
    }
    return false;
}


/* ================================================== *
 * Debug functions
 * ================================================== */
#ifdef DH_DEBUG

// Based on: https://github.com/raspberrypi/pico-sdk/blob/a1438dff1d38bd9c65dbd693f0e5db4b9ae91779/src/rp2_common/pico_stdio_usb/stdio_usb.c#L100-L130
static void cdc_write_str(const char *str) {
    int str_len = strlen(str);

    if (!tud_cdc_connected())
        return;

    uint64_t last_write_time = time_us_64();

    for (int bytes_written = 0; bytes_written < str_len;) {
        int bytes_remaining = str_len - bytes_written;
        int available_space = (int)tud_cdc_write_available();
        int chunk_size      = (bytes_remaining < available_space) ? bytes_remaining : available_space;

        if (chunk_size > 0) {
            int written = (int)tud_cdc_write(str + bytes_written, (uint32_t)chunk_size);
            tud_task();
            tud_cdc_write_flush();

            bytes_written += written;
            last_write_time = time_us_64();
        } else {
            tud_task();
            tud_cdc_write_flush();

            /* Timeout after 1ms if buffer stays full or connection lost */
            if (!tud_cdc_connected() || (time_us_64() > last_write_time + 1000))
                break;
        }
    }
}


int dh_debug_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    char buffer[512];

    int string_len = vsnprintf(buffer, 512, format, args);

    cdc_write_str(buffer);
    tud_cdc_write_flush();

    va_end(args);
    return string_len;
}
#else

int dh_debug_printf(const char *format, ...) {
    return 0;
}

#endif
