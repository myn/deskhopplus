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

#include "config_read_all.h"
#include "dh_hotkey.h"
#include "dh_pair.h"

#include <stddef.h>
#include <stdint.h>
#include "config_layout.h"
#include "dh_txq.h"
#include "flash.h"
#include "fw_upgrade.h"
#include "packet.h"
#include "peer_fw.h"
#include "screen.h"

typedef void (*action_handler_t)();

typedef struct { // Maps message type -> message handler function
    enum packet_type_e type;
    action_handler_t handler;
} uart_handler_t;

typedef dh_hotkey_t hotkey_combo_t;

_Static_assert(KEYS_IN_USB_REPORT == DH_HOTKEY_KEY_CAPACITY,
               "core and firmware keyboard reports must have the same key capacity");

typedef struct TU_ATTR_PACKED {
    uint8_t buttons;
    int16_t x;
    int16_t y;
    int8_t wheel;
    int8_t pan;
    uint8_t mode;
} mouse_report_t;

typedef struct {
    uint8_t tip_pressure;
    uint8_t buttons; // Digitizer buttons
    uint16_t x;      // X coordinate (0-32767)
    uint16_t y;      // Y coordinate (0-32767)
} touch_report_t;

typedef struct {
    uint8_t instance;
    uint8_t report_id;
    uint8_t type;
    uint8_t len;
    uint8_t data[RAW_PACKET_LENGTH];
} hid_generic_pkt_t;

typedef enum { IDLE, READING_PACKET, PROCESSING_PACKET } receiver_state_t;

/* fw_upgrade_state_t lives in fw_upgrade.h — split out so the decision that a
   transfer has stalled can be tested on the host (#90).

   config_t, its magic and its version live in config_layout.h — split out so
   the validation in config_store.h can be tested on the host (#74). */


/*==============================================================================
 *  Device State
 *==============================================================================*/
typedef enum {
    CURSOR_CROSSING_IDLE = 0,
    CURSOR_CROSSING_WAITING,
    CURSOR_CROSSING_REANCHORED,
    CURSOR_CROSSING_FALLBACK,
    CURSOR_CROSSING_CANCELLED,
    CURSOR_CROSSING_RESUMING,
} cursor_crossing_phase_t;

typedef enum {
    CURSOR_CROSSING_SOURCE_REANCHOR = 0,
    CURSOR_CROSSING_MACOS_PLACEMENT,
} cursor_crossing_kind_t;

typedef struct {
    cursor_crossing_phase_t phase;
    cursor_crossing_kind_t kind;
    uint8_t direction;
    uint8_t output;
    uint8_t query_id;
    uint8_t target_screen;
    uint16_t target_position;
    bool query_sent;
    uint32_t started_us;
} cursor_crossing_t;

typedef struct {
    uint8_t kbd_dev_addr; // Address of the Keyboard device
    uint8_t kbd_instance; // Keyboard instance (d'uh - isn't this a useless comment)

    uint8_t keyboard_leds_desired[NUM_SCREENS];  // Desired state of keyboard LEDs (index 0 = A, index 1 = B)
    uint8_t keyboard_leds_actual[NUM_SCREENS];   // Actual state of keyboard LEDs
    uint64_t last_activity[NUM_SCREENS]; // Timestamp of the last input activity (-||-)
    uint32_t core1_last_loop_pass;       // Timestamp of last core1 loop execution
    uint8_t active_output;               // Currently selected output (0 = A, 1 = B)
    uint8_t board_role;                  // Which board are we running on? (0 = A, 1 = B, etc.)

    hid_keyboard_report_t local_kbd_states[MAX_DEVICES]; // Store keyboard states
    hid_keyboard_report_t remote_kbd_state;              // Store combined remote keyboard state
    uint8_t max_kbd_idx;                                 // Store largest kbd_idx seen

    int16_t pointer_x; // Store and update the location of our mouse pointer
    int16_t pointer_y;
    int16_t mouse_buttons; // Store and update the state of mouse buttons

    config_t config;            // Device configuration, loaded from flash or defaults used
    queue_t hid_queue_out;      // Queue that stores outgoing hid messages
    queue_t kbd_queue;          // Queue that stores keyboard reports
    queue_t mouse_queue;        // Queue that stores mouse reports
    queue_t uart_tx_queue;      // Queue that stores outgoing packets
    dh_txq_stats uart_tx_stats; // Drops from a full uart_tx_queue (#43)

    hid_interface_t iface[MAX_DEVICES][MAX_INTERFACES]; // Store info about HID interfaces
    uart_packet_t in_packet;

    /* DMA */
    uint32_t dma_ptr;             // Stores info about DMA ring buffer last checked position
    uint32_t dma_rx_channel;      // DMA RX channel we're using to receive
    uint32_t dma_control_channel; // DMA channel that controls the RX transfer channel
    uint32_t dma_tx_channel;      // DMA TX channel we're using to send

    /* Firmware */
    fw_upgrade_state_t fw;           // State of the firmware upgrader
    firmware_metadata_t _running_fw; // RAM copy of running fw metadata
    peer_fw_t peer_fw;               // What the other board reports running (#89)

    /*
     * What the helper channel has dropped, since boot, published here by
     * channel_task so the config API can read it (fields 91-97).
     *
     * The counters themselves live in channel.c, where the seams are. They are
     * copied out rather than moved because each one belongs to the code that
     * increments it — and every one of them was already counted (#43) but
     * readable nowhere, which is the same "silent because nobody can see it"
     * that #94 cost two days to.
     *
     * The config page is not where these are worth reading: it is reachable
     * only in config mode, which is entered by rebooting the board that holds
     * them, so it can only ever be shown a board that has just zeroed them
     * (#133). The live reading goes to the helper over the channel, in
     * DEVICE_DROPS — channel_task sets both from the same values.
     */
    uint32_t _channel_reports_dropped;
    uint32_t _channel_inbound_dropped;
    uint32_t _channel_outq_refused;
    uint32_t _channel_relay_dropped;
    /*
     * The inter-board reassembler's own two, and they are the ones that were
     * missing when #52's first size-dependent fault had to be diagnosed. A
     * longer payload is more packets, so it is more chances to lose one — and
     * a frame that loses a packet is abandoned whole, silently, with every
     * counter on the *sending* board still reading zero.
     */
    uint32_t _channel_relay_orphans;
    uint32_t _channel_relay_truncated;
    uint32_t _channel_relay_refused;
    bool dev_build;                  // True when channel authentication is compiled out (#44)
    bool reboot_requested;           // If set, stop updating watchdog
    uint64_t config_mode_timer;      // Counts how long are we to remain in config mode

    uint8_t page_buffer[CONFIG_FLASH_BYTES]; // Config save and firmware page staging

    /* Connection status flags */
    bool tud_connected;      // True when TinyUSB device successfully connects
    bool keyboard_connected; // True when our keyboard is connected locally
    bool mouse_connected;    // True when our mouse is connected locally

    /* Feature flags */
    bool mouse_zoom;         // True when "mouse zoom" is enabled
    bool switch_lock;        // True when device is prevented from switching
    bool onboard_led_state;  // True when LED is ON
    bool relative_mouse;     // True when relative mouse mode is used
    bool gaming_mode;        // True when gaming mode is on (relative passthru + lock)
    bool config_mode_active; // True when config mode is active
    bool digitizer_active;   // True when digitizer Win/Mac workaround is active

    /* Where a config Read All has got to, walked one field per HID queue
       drain so the map never has to fit in the queue (#156). */
    config_read_all_t config_read_all;

    /* A relative-mode output crossing waiting for the source helper's true
       cursor position before its seam mapping is resolved (#30). */
    cursor_crossing_t cursor_crossing;
    uint8_t output_arrival_guard;
    uint16_t output_arrival_reverse;
    uint8_t next_cursor_query_id;

    /* Onboard LED blinky (provide feedback when e.g. mouse connected) */
    int32_t  blinks_left;     // How many blink transitions are left
    uint32_t last_led_change; // Timestamp of the last time led state transitioned
} device_t;
/*==============================================================================*/


typedef struct {
    void (*exec)(device_t *state);
    uint64_t frequency;
    uint64_t next_run;
    bool *enabled;
} task_t;

enum screen_pos_e {
    NONE   = DH_DIRECTION_NONE,
    LEFT   = DH_DIRECTION_LEFT,
    RIGHT  = DH_DIRECTION_RIGHT,
    MIDDLE = 3,
    TOP    = DH_DIRECTION_TOP,
    BOTTOM = DH_DIRECTION_BOTTOM,
};

enum screensaver_mode_e {
    DISABLED   = 0,
    PONG       = 1,
    JITTER     = 2,
    MAX_SS_VAL = JITTER,
};

extern const config_t default_config;
extern const config_t ADDR_CONFIG[];
extern const uint8_t ADDR_IDENTITY[];
extern const uint8_t ADDR_FW_METADATA[];
extern const uint8_t ADDR_FW_RUNNING[];
extern const uint8_t ADDR_FW_STAGING[];
extern const uint8_t ADDR_DISK_IMAGE[];
