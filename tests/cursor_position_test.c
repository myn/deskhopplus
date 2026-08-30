#include <stdio.h>

#include "main.h"

device_t global_state;
static int failures;

#define CHECK(condition, message) do { if (!(condition)) { \
    fprintf(stderr, "FAIL: %s\n", message); failures++; } } while (0)

static device_t two_monitor_macos(void) {
    device_t state = {0};
    state.config.output[0].screen_count = 2;
    state.config.output[0].os = MACOS;
    return state;
}

static void receive_position(device_t *state, uint8_t output, uint8_t screen,
                             uint16_t x, uint16_t y) {
    uart_packet_t packet = {.data = {output, screen}};
    packet.data16[1] = x;
    packet.data16[2] = y;
    handle_cursor_position_msg(&packet, state);
}

static void test_secondary_helper_position_selects_relative_reports(void) {
    device_t state = two_monitor_macos();
    receive_position(&state, 0, 2, 12000, 16000);
    CHECK(state.config.output[0].screen_index == 2,
          "helper position did not select monitor 2");
    CHECK(dh_mouse_reports_are_relative(state.relative_mouse, state.gaming_mode),
          "helper placement on monitor 2 left the next report absolute");
}

static void test_primary_helper_position_restores_absolute_reports(void) {
    device_t state = two_monitor_macos();
    state.relative_mouse = true;
    receive_position(&state, 0, 1, 12000, 16000);
    CHECK(!dh_mouse_reports_are_relative(state.relative_mouse, state.gaming_mode),
          "helper placement on the primary monitor left the next report relative");
}

static void test_stale_inactive_output_position_is_ignored(void) {
    device_t state = two_monitor_macos();
    state.pointer_x = 7000;
    state.pointer_y = 8000;
    state.config.output[1].screen_count = 2;
    state.config.output[1].os = WINDOWS;

    receive_position(&state, 1, 2, 12000, 16000);

    CHECK(state.pointer_x == 7000 && state.pointer_y == 8000,
          "inactive output response rewound the active cursor");
    CHECK(!dh_mouse_reports_are_relative(state.relative_mouse, state.gaming_mode),
          "inactive output response changed the active report mode");
}

int main(void) {
    test_secondary_helper_position_selects_relative_reports();
    test_primary_helper_position_restores_absolute_reports();
    test_stale_inactive_output_position_is_ignored();
    if (failures) return 1;
    puts("cursor_position_test: all checks passed");
    return 0;
}
