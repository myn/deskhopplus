#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dh_mouse_layout.h"
#include "dh_cursor_trace.h"
#include "screen.h"

#define NUM_SCREENS 2
#define MIN_SCREEN_COORD 0
#define MAX_SCREEN_COORD 32767
#define MOUSE_ZOOM_SCALING_FACTOR 2

enum screen_pos_e {
    NONE = DH_DIRECTION_NONE,
    LEFT = DH_DIRECTION_LEFT,
    RIGHT = DH_DIRECTION_RIGHT,
    TOP = DH_DIRECTION_TOP,
    BOTTOM = DH_DIRECTION_BOTTOM,
};

typedef struct {
    int32_t move_x;
    int32_t move_y;
    int32_t wheel;
    int32_t pan;
    int32_t buttons;
} mouse_values_t;

typedef enum {
    CURSOR_CROSSING_IDLE = 0,
    CURSOR_CROSSING_WAITING,
    CURSOR_CROSSING_REANCHORED,
    CURSOR_CROSSING_FALLBACK,
    CURSOR_CROSSING_CANCELLED,
    CURSOR_CROSSING_RESUMING,
} cursor_crossing_phase_t;

typedef struct {
    cursor_crossing_phase_t phase;
    uint8_t direction;
    uint8_t output;
    uint8_t query_id;
    uint32_t started_us;
} cursor_crossing_t;

typedef struct {
    struct {
        uint16_t jump_threshold;
        uint8_t enable_acceleration;
        output_t output[NUM_SCREENS];
    } config;
    uint8_t active_output;
    int16_t pointer_x;
    int16_t pointer_y;
    int16_t mouse_buttons;
    bool mouse_zoom;
    bool switch_lock;
    bool gaming_mode;
    bool relative_mouse;
    cursor_crossing_t cursor_crossing;
    uint8_t next_cursor_query_id;
} device_t;

typedef struct {
    uint8_t type;
    union { uint8_t data[8]; uint16_t data16[4]; };
    uint8_t checksum;
} uart_packet_t;

#define OUTPUT_A 0
#define OUTPUT_B 1
static inline uint32_t time_us_32(void) { return 0; }
typedef int critical_section_t;
static inline void critical_section_init(critical_section_t *lock) { (void)lock; }
static inline void critical_section_enter_blocking(critical_section_t *lock) { (void)lock; }
static inline void critical_section_exit(critical_section_t *lock) { (void)lock; }

extern device_t global_state;

void switch_to_another_pc(device_t *, output_t *, int, int);
void switch_virtual_desktop(device_t *, output_t *, int, int);
void channel_place_cursor(uint8_t, uint8_t, uint8_t, uint8_t, uint16_t);
bool channel_query_cursor(uint8_t, uint8_t);
void mouse_crossing_task(device_t *, uint32_t);
void mouse_crossing_query_unavailable(device_t *, uint8_t, uint8_t);
bool apply_helper_cursor_position(device_t *, uint8_t, uint8_t, int16_t, int16_t, uint8_t);
bool select_cursor_screen(device_t *, uint8_t, uint8_t);
void cursor_crossing_init(void);
void cursor_crossing_enter(void);
void cursor_crossing_exit(void);
void handle_cursor_position_msg(uart_packet_t *, device_t *);
void cursor_trace_event(const device_t *, dh_cursor_trace_event_t, uint8_t,
                        int16_t, int16_t, uint8_t, uint8_t);
