#include <stdio.h>

#include "main.h"

device_t global_state;
static int failures;

void mouse_crossing_query_unavailable(device_t *state, uint8_t output, uint8_t query_id) {
    (void)state;
    (void)output;
    (void)query_id;
}

#define CHECK(condition, message) do { if (!(condition)) { \
    fprintf(stderr, "FAIL: %s\n", message); failures++; } } while (0)

static device_t two_monitor_output(uint8_t os) {
    device_t state = {0};
    state.config.output[0].screen_count = 2;
    state.config.output[0].os = os;
    return state;
}

static void receive_position(device_t *state, uint8_t output, uint8_t screen,
                             uint16_t x, uint16_t y) {
    uart_packet_t packet = {.data = {output, screen}};
    packet.data16[1] = x;
    packet.data16[2] = y;
    packet.data[6] = 0;
    handle_cursor_position_msg(&packet, state);
}

static void test_windows_secondary_helper_position_selects_relative_reports(void) {
    device_t state = two_monitor_output(WINDOWS);
    state.config.output[0].screen_index = 2; /* selected by the placement */
    receive_position(&state, 0, 2, 12000, 16000);
    CHECK(state.config.output[0].screen_index == 2,
          "helper position did not select monitor 2");
    CHECK(dh_mouse_reports_are_relative(state.relative_mouse, state.gaming_mode),
          "helper placement on monitor 2 left the next report absolute");
}

static void test_macos_secondary_helper_position_keeps_absolute_reports(void) {
    device_t state = two_monitor_output(MACOS);
    state.config.output[0].screen_index = 2;
    state.relative_mouse = true; /* mode carried from a Windows secondary */
    receive_position(&state, 0, 2, 12000, 16000);
    CHECK(!dh_mouse_reports_are_relative(state.relative_mouse, state.gaming_mode),
          "macOS helper placement inherited Windows relative report mode");
}

static void test_primary_helper_position_restores_absolute_reports(void) {
    device_t state = two_monitor_output(MACOS);
    state.config.output[0].screen_index = 1;
    state.relative_mouse = true;
    receive_position(&state, 0, 1, 12000, 16000);
    CHECK(!dh_mouse_reports_are_relative(state.relative_mouse, state.gaming_mode),
          "helper placement on the primary monitor left the next report relative");
}

static void test_uncorrelated_placement_readback_cannot_rewind_selected_screen(void) {
    device_t state = two_monitor_output(MACOS);
    state.config.output[0].screen_index = 1;
    state.pointer_x = 0;
    state.pointer_y = MAX_SCREEN_COORD;

    receive_position(&state, 0, 2, 0, 32000);

    CHECK(state.config.output[0].screen_index == 1,
          "uncorrelated placement readback rewound the selected monitor");
    CHECK(state.pointer_x == 0 && state.pointer_y == MAX_SCREEN_COORD,
          "uncorrelated placement readback rewound the selected coordinates");
}

static void test_stale_inactive_output_position_is_ignored(void) {
    device_t state = two_monitor_output(MACOS);
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

static void test_invalid_output_position_is_ignored(void) {
    device_t state = two_monitor_output(MACOS);
    state.config.output[0].screen_index = 1;
    state.pointer_x = 7000;
    state.pointer_y = 8000;

    receive_position(&state, 2, 1, 12000, 16000);

    CHECK(state.config.output[0].screen_index == 1,
          "invalid output response changed the selected monitor");
    CHECK(state.pointer_x == 7000 && state.pointer_y == 8000,
          "invalid output response rewound the cursor");
}

int main(void) {
    test_windows_secondary_helper_position_selects_relative_reports();
    test_macos_secondary_helper_position_keeps_absolute_reports();
    test_primary_helper_position_restores_absolute_reports();
    test_stale_inactive_output_position_is_ignored();
    test_invalid_output_position_is_ignored();
    test_uncorrelated_placement_readback_cannot_rewind_selected_screen();
    if (failures) return 1;
    puts("cursor_position_test: all checks passed");
    return 0;
}
