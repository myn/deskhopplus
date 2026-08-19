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

/*********  Global Variables  **********/
device_t global_state     = {0};
device_t *device          = &global_state;

firmware_metadata_t _firmware_metadata __attribute__((section(".section_metadata"))) = {
    .version = 0x0001,
};

/* ================================================== *
 * ==============  Main Program Loops  ============== *
 * ================================================== */

int main(void) {
    static task_t tasks_core0[] = {
        [0] = {.exec = &usb_device_task,          .frequency = _TOP()},      // .-> USB device task, needs to run as often as possible
        [1] = {.exec = &kick_watchdog_task,       .frequency = _HZ(30)},     // | Verify core1 is still running and if so, reset watchdog timer
        [2] = {.exec = &process_kbd_queue_task,   .frequency = _HZ(2000)},   // | Check if there were any keypresses and send them
        [3] = {.exec = &process_mouse_queue_task, .frequency = _HZ(2000)},   // | Check if there were any mouse movements and send them
        [4] = {.exec = &process_hid_queue_task,   .frequency = _HZ(1000)},   // | Check if there are any packets to send over vendor link
        [5] = {.exec = &process_uart_tx_task,     .frequency = _TOP()},      // | Check if there are any packets to send over UART
        [6] = {.exec = &channel_task,             .frequency = _HZ(1000)},   // | Answer the helper on the channel and time out a silent one
#ifdef DH_BENCH_ECDH
        [7] = {.exec = &bench_ecdh_task,          .frequency = _HZ(100)},    // | MEASURE-ONLY BUILD: time a P-256 ECDH and type the answer (#110)
#endif
    };                                                                       // `----- then go back and repeat forever
    const int NUM_TASKS = ARRAY_SIZE(tasks_core0);

    // Wait for the board to settle
    sleep_ms(10);

    // Initial board setup
    initial_setup(device);

    // Initial state, A is the default output
    set_active_output(device, OUTPUT_A);

    while (true) {
        for (int i = 0; i < NUM_TASKS; i++)
            task_scheduler(device, &tasks_core0[i]);
    }
}

void core1_main() {
    static task_t tasks_core1[] = {
        [0] = {.exec = &usb_host_task,           .frequency = _TOP()},       // .-> USB host task, needs to run as often as possible
        [1] = {.exec = &packet_receiver_task,    .frequency = _TOP()},       // | Receive data over serial from the other board
        [2] = {.exec = &led_blinking_task,       .frequency = _HZ(30)},      // | Check if LED needs blinking
        [3] = {.exec = &led_sync_task,           .frequency = _HZ(30)},      // | Sync LED state if needed
        [4] = {.exec = &screensaver_task,        .frequency = _HZ(120)},     // | Handle "screensaver" movements
        [5] = {.exec = &firmware_upgrade_task,   .frequency = _HZ(4000)},    // | Send firmware to the other board if needed
#ifndef DH_BENCH_ECDH
        [6] = {.exec = &heartbeat_output_task,   .frequency = _HZ(1)},       // | Output periodic heartbeats
#endif                                                                       // | (silenced in the measure-only build -- see below)
    };                                                                       // `----- then go back and repeat forever
    const int NUM_TASKS = ARRAY_SIZE(tasks_core1);

    while (true) {
        // Update the timestamp, so core0 can figure out if we're dead
        device->core1_last_loop_pass = time_us_32();

        for (int i = 0; i < NUM_TASKS; i++)
            task_scheduler(device, &tasks_core1[i]);
    }
}
/*
 * Why the measure-only build (#110) has no heartbeat.
 *
 * The heartbeat is what carries this board's firmware version and checksum to
 * its peer, and handle_heartbeat_msg on the far side pulls the image whenever
 * it differs -- on a strictly newer version, or on an *equal* version carrying
 * a different image (#91). The measuring image is exactly that: same version,
 * different bytes. Left announcing, it would propagate itself onto the peer
 * board, and getting the real firmware back onto board B costs a cable trip.
 *
 * So it stays quiet. The peer simply does not learn what this board is
 * running, for as long as the measurement takes.
 */
/* =======  End of Main Program Loops  ======= */
