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

void task_scheduler(device_t *state, task_t *task) {
    uint64_t current_time = time_us_64();

    if (current_time < task->next_run)
        return;

    task->next_run = current_time + task->frequency;
    task->exec(state);
}

/* ================================================== *
 * ==============  Watchdog Functions  ============== *
 * ================================================== */

void kick_watchdog_task(device_t *state) {
    /* Read the timer AFTER duplicating the core1 timestamp,
       so it doesn't get updated in the meantime. */
    uint32_t core1_last_loop_pass = state->core1_last_loop_pass;
    uint32_t current_time         = time_us_32();

    /* If a reboot is requested, we'll stop updating watchdog */
    if (state->reboot_requested)
        return;

    /* If core1 stops updating the timestamp, we'll stop kicking the watchog and reboot */
    if ((uint32_t)(current_time - core1_last_loop_pass) < CORE1_HANG_TIMEOUT_US)
        watchdog_update();
}

/* ================================================== *
 * ===============  USB Device / Host  ============== *
 * ================================================== */

void usb_device_task(device_t *state) {
    tud_task();
}

void usb_host_task(device_t *state) {
    if (tuh_inited())
        tuh_task();
}

mouse_report_t *screensaver_pong(device_t *state) {
    static mouse_report_t report = {0};
    static int dx = 20, dy = 25;

    /* Check if we are bouncing off the walls and reverse direction in that case. */
    if (report.x + dx < MIN_SCREEN_COORD || report.x + dx > MAX_SCREEN_COORD)
        dx = -dx;

    if (report.y + dy < MIN_SCREEN_COORD || report.y + dy > MAX_SCREEN_COORD)
        dy = -dy;

    report.x += dx;
    report.y += dy;

    return &report;
}

mouse_report_t *screensaver_jitter(device_t *state) {
    static mouse_report_t report = {
        .y = JITTER_DISTANCE,
        .mode = RELATIVE,
    };
    report.y = -report.y;

    return &report;
}

/* Have something fun and entertaining when idle. */
void screensaver_task(device_t *state) {
    const uint32_t delays[] = {
        0,        /* DISABLED, unused index 0 */
        5000,     /* PONG, move mouse every 5 ms for a high framerate */
        10000000, /* JITTER, once every 10 sec is more than enough */
    };
    static uint32_t last_pointer_move = 0;
    screensaver_t *screensaver = &state->config.output[BOARD_ROLE].screensaver;
    uint64_t inactivity_period = time_us_64() - state->last_activity[BOARD_ROLE];

    /* If we're not enabled, nothing to do here. */
    if (screensaver->mode == DISABLED)
        return;

    /* System is still not idle for long enough to activate or screensaver mode is not supported */
    if (inactivity_period < screensaver->idle_time_us || screensaver->mode > MAX_SS_VAL)
        return;

    /* We exceeded the maximum permitted screensaver runtime */
    if (screensaver->max_time_us
        && inactivity_period > (screensaver->max_time_us + screensaver->idle_time_us))
        return;

    /* If we're the selected output and we can only run on inactive output, nothing to do here. */
    if (screensaver->only_if_inactive && CURRENT_BOARD_IS_ACTIVE_OUTPUT)
        return;

    /* We're active! Now check if it's time to move the cursor yet. */
    if (time_us_32() - last_pointer_move < delays[screensaver->mode])
        return;

    /* Return, if we're not connected or the host is suspended */
    if(!tud_ready()) {
        return;
    }

    mouse_report_t *report;
    switch (screensaver->mode) {
        case PONG:
            report = screensaver_pong(state);
            break;

        case JITTER:
            report = screensaver_jitter(state);
            break;

        default:
            return;
    }

    /* Move mouse pointer */
    queue_mouse_report(report, state);

    /* Update timer of the last pointer move */
    last_pointer_move = time_us_32();
}

/* Periodically emit heartbeat packets.
 *
 * This task used to return early on upgrade_in_progress — a flag only
 * completion cleared — which took the heartbeat, the config-mode timeout and
 * the config-mode LED down with any transfer that stopped. The board went
 * silent to its peer board, could not retry, could not leave config mode, and
 * looked healthy throughout. Only a power cycle got it back (#90).
 *
 * The guard's own justification was "don't touch flash_cs", and exactly one
 * thing here does that: is_bootsel_pressed, which drives the QSPI chip select
 * by hand. That call keeps a guard; the rest no longer have one, because
 * queueing a packet and setting a blink counter touch no flash at all.
 *
 * The config-mode reboot keeps a guard too, for a different reason given at
 * the call. Both remaining guards are now bounded by FW_UPGRADE_STALL_US,
 * which is what stops a transfer that stopped from holding either forever.
 */
void heartbeat_output_task(device_t *state) {
    uint64_t now = time_us_64();

    /* Forget a peer board that has stopped heartbeating, so its version is not
       left reading as current after it has gone (#89). This task's own cadence
       is the heartbeat interval, which is what the staleness window counts. */
    peer_fw_expire(&state->peer_fw, now);

    /* Give up on a transfer that has gone quiet, so the board can start it
       again instead of waiting for a power cycle. Restarting a pull rewrites
       every page, so this is also how a half-written image gets repaired —
       see abandon_firmware_upgrade for when it stops being worth trying.

       The 32-bit clock rather than `now` is deliberate, not an oversight: the
       UF2 path stamps that timestamp from core0, and fw_upgrade.h explains why
       a 64-bit one could tear across cores. */
    if (fw_upgrade_stalled(&state->fw, time_us_32()))
        abandon_firmware_upgrade(state);

    if (state->config_mode_active) {
        /* Leave config mode if timeout expired and user didn't click exit.
           A live upgrade still defers this, because the UF2 disk only exists
           in config mode and rebooting mid-write would brick the board — but
           "live" is now bounded by FW_UPGRADE_STALL_US, so a stalled transfer
           can hold config mode open for that long and no longer. */
        if (now > state->config_mode_timer && !state->fw.upgrade_in_progress)
            reboot();

        /* Keep notifying the user we're still in config mode. Skipping this
           was what made a stalled board look like it had left config mode:
           restore_leds went unopposed and the LED reverted to the normal-mode
           indicator, which is the opposite of the truth. */
        blink_led(state);
    }

#ifdef DH_DEBUG
    /* Holding the button invokes bootsel firmware upgrade. This is the
       flash_cs toucher the original guard was written for, so it keeps one:
       the UF2 path writes flash from core0 while this runs on core1. */
    if (!state->fw.upgrade_in_progress && is_bootsel_pressed())
        reset_usb_boot(1 << PICO_DEFAULT_LED_PIN, 0);
#endif

    uart_packet_t packet = {
        .type = HEARTBEAT_MSG,
        .data16 = {
            [0] = state->_running_fw.version,
            [2] = state->active_output,
        },
    };

    (void)queue_uart_packet(&packet, state);
}


/* Process other outgoing hid report messages. */
void process_hid_queue_task(device_t *state) {
    hid_generic_pkt_t packet;

    if (!queue_try_peek(&state->hid_queue_out, &packet))
        return;

    if (!tud_hid_n_ready(packet.instance))
        return;

    /* ... try sending it to the host, if it's successful */
    bool succeeded = tud_hid_n_report(packet.instance, packet.report_id, packet.data, packet.len);

    /* ... then we can remove it from the queue. Race conditions shouldn't happen [tm] */
    if (succeeded)
        queue_try_remove(&state->hid_queue_out, &packet);
}

/* Task that handles copying firmware from the other device to ours */
void firmware_upgrade_task(device_t *state) {
    if (!state->fw.upgrade_in_progress || !state->fw.byte_done)
        return;

    if (queue_is_full(&state->uart_tx_queue))
        return;

    /* End condition, when reached the process is completed. */
    if (state->fw.address > STAGING_IMAGE_SIZE) {
        state->fw.upgrade_in_progress = 0;
        state->fw.checksum = ~state->fw.checksum;

        /* Checksum mismatch, we wipe the stage 2 bootloader and rely on ROM recovery */
        if(calculate_firmware_crc32() != state->fw.checksum)
            recover_to_rom();
        else {
            /* The image is whole again, so nothing is left to repair. */
            state->fw.image_dirty      = false;
            state->fw.repair_attempted = false;
            state->_running_fw = _firmware_metadata;
            global_state.reboot_requested = true;
        }
    }

    /* If we're on the last element of the current page, page is done - write it. */
    if (TU_U32_BYTE0(state->fw.address) == 0x00) {

        uint32_t page_start_addr = (state->fw.address - 1) & 0xFFFFFF00;
        write_flash_page((uint32_t)ADDR_FW_RUNNING + page_start_addr - XIP_BASE, state->page_buffer);

        /* From here on the running image is part-old and part-new, so stopping
           short of the end must not leave the board booting it (#90). */
        state->fw.image_dirty = true;
    }

    request_byte(state, state->fw.address);
}

void packet_receiver_task(device_t *state) {
    uint32_t current_pointer
        = (uint32_t)DMA_RX_BUFFER_SIZE - dma_channel_hw_addr(state->dma_rx_channel)->transfer_count;
    uint32_t delta = get_ptr_delta(current_pointer, state);

    /* If we don't have enough characters for a packet, skip loop and return immediately */
    while (delta >= RAW_PACKET_LENGTH) {
        if (is_start_of_packet(state)) {
            fetch_packet(state);
            process_packet(&state->in_packet, state);
            return;
        }

        /* No packet found, advance to next position and decrement delta */
        state->dma_ptr = NEXT_RING_IDX(state->dma_ptr);
        delta--;
    }
}
