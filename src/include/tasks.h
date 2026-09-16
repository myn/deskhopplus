/*
 * This file is part of DeskHop (https://github.com/hrvach/deskhop).
 * Copyright (c) 2025 Hrvoje Cavrak
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * See the file LICENSE for the full license text.
 * Modified by Derek Reynolds, 2026, for deskhopplus.
 */
#pragma once

#include "structs.h"

/*==============================================================================
 *  Core Task Scheduling
 *==============================================================================*/

 void task_scheduler(device_t *, task_t *);

/*==============================================================================
 *  Individual Task Functions
 *==============================================================================*/

void channel_task(device_t *);
void firmware_upgrade_task(device_t *);
void heartbeat_output_task(device_t *);
void kick_watchdog_task(device_t *);
void led_blinking_task(device_t *);
void led_sync_task(device_t *);
void packet_receiver_task(device_t *);
void process_hid_queue_task(device_t *);
void process_kbd_queue_task(device_t *);
void process_mouse_queue_task(device_t *);
void process_uart_tx_task(device_t *);
void screensaver_task(device_t *);
void usb_device_task(device_t *);
void usb_host_task(device_t *);

/* The board's own USB host port, for usb_host_task's replug watch (#102).
   Defined in usb.c beside the host callbacks. */
bool usb_host_attached(void);
bool usb_host_any_mounted(void);
void usb_host_replug(void);

#ifdef DH_BENCH_ECDH
/* Measure-only build (-DDH_BENCH_ECDH=ON). Not part of the product: see
   src/bench_ecdh.c and tools/board-checks/README.md. */
void bench_ecdh_task(device_t *);
#endif

#ifdef DH_BENCH_UART
/* Measure-only build (-DDH_BENCH_UART=ON). Not part of the product: see
   src/bench_uart.c and tools/board-checks/README.md. */
void bench_uart_task(device_t *);
#endif
