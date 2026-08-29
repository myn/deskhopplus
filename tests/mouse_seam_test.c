#include <stdio.h>
#include <string.h>

#include "main.h"

device_t global_state;
static int output_switches;
static int virtual_switches;
static int requested_index;

enum screen_pos_e update_mouse_position(device_t *, mouse_values_t *);
void do_screen_switch(device_t *, int);

void switch_to_another_pc(device_t *state, output_t *output, int output_to, int direction) {
    (void)output;
    (void)direction;
    output_switches++;
    state->active_output = (uint8_t)output_to;
}

void switch_virtual_desktop(device_t *state, output_t *output, int new_index, int direction) {
    (void)state;
    (void)direction;
    virtual_switches++;
    requested_index = new_index;
    output->screen_index = (uint32_t)new_index;
}

static int failures;
#define CHECK(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL mouse_seam: %s\n", message); failures++; } \
} while (0)

/* edge_map.h's original double-and-round formulas, kept independent of firmware math. */
static uint16_t mkroamer_cross(uint16_t position, uint16_t start, uint16_t end) {
    const double fraction = (double)(position - start) / (double)(end - start);
    return (uint16_t)(fraction * 65535.0 + 0.5);
}

static uint16_t mkroamer_entry(uint16_t position, uint16_t start, uint16_t end) {
    return (uint16_t)(start + ((double)position / 65535.0) * (end - start) + 0.5);
}

static device_t side_by_side_state(void) {
    device_t state;
    memset(&state, 0, sizeof state);
    state.pointer_x = 10;
    state.pointer_y = 100;
    state.config.jump_threshold = 5;
    state.config.output[0] = (output_t){
        .number = 0, .screen_count = 2, .screen_index = 1,
        .speed_x = 1, .speed_y = 1,
        .chain_direction = DH_DIRECTION_RIGHT,
        .border_direction = DH_DIRECTION_LEFT,
    };
    state.config.output[1] = (output_t){
        .number = 1, .screen_count = 3, .screen_index = 1,
        .speed_x = 1, .speed_y = 1,
        .chain_direction = DH_DIRECTION_LEFT,
        .border_direction = DH_DIRECTION_RIGHT,
    };
    return state;
}

static device_t stacked_computers_state(void) {
    device_t state = side_by_side_state();
    state.pointer_x = 100;
    state.pointer_y = 10;
    state.config.output[0].screen_index = 2;
    state.config.output[0].border_direction = DH_DIRECTION_TOP;
    return state;
}

static void test_perpendicular_seam_crosses_from_any_monitor(void) {
    device_t state = stacked_computers_state();
    global_state = state;
    mouse_values_t movement = {.move_y = -16};

    enum screen_pos_e direction = update_mouse_position(&state, &movement);
    CHECK(direction == TOP, "update_mouse_position did not detect the top seam");
    CHECK(state.pointer_y == MIN_SCREEN_COORD, "update_mouse_position did not clamp Y");

    output_switches = 0;
    do_screen_switch(&state, direction);
    CHECK(output_switches == 1 && state.active_output == 1,
          "perpendicular seam did not cross from a non-primary monitor");

    state = stacked_computers_state();
    state.pointer_y = MAX_SCREEN_COORD - 10;
    state.config.output[0].border_direction = DH_DIRECTION_BOTTOM;
    global_state = state;
    movement.move_y = 16;
    direction = update_mouse_position(&state, &movement);
    CHECK(direction == BOTTOM, "update_mouse_position did not detect the bottom seam");

    output_switches = 0;
    do_screen_switch(&state, direction);
    CHECK(output_switches == 1 && state.active_output == 1,
          "bottom seam did not cross from a non-primary monitor");
}

static void test_diagonal_push_uses_the_larger_overshoot(void) {
    device_t state = stacked_computers_state();
    state.pointer_x = 2;
    state.pointer_y = 2;
    global_state = state;
    mouse_values_t movement = {.move_x = -10, .move_y = -20};

    enum screen_pos_e direction = update_mouse_position(&state, &movement);
    CHECK(direction == TOP, "diagonal push did not choose the farther Y overshoot");

    state.pointer_x = 2;
    state.pointer_y = 2;
    movement.move_x = -12;
    movement.move_y = -14;
    CHECK(update_mouse_position(&state, &movement) == TOP,
          "jump threshold distorted the corner overshoot comparison");
}

static void test_vertical_seam_preserves_crossing_guards(void) {
    device_t state = stacked_computers_state();

    output_switches = 0;
    state.switch_lock = true;
    do_screen_switch(&state, TOP);
    CHECK(output_switches == 0, "switch lock allowed a vertical crossing");

    state.switch_lock = false;
    state.gaming_mode = true;
    do_screen_switch(&state, TOP);
    CHECK(output_switches == 0, "gaming mode allowed a vertical crossing");

    state.gaming_mode = false;
    state.mouse_buttons = 1;
    do_screen_switch(&state, TOP);
    CHECK(output_switches == 0, "held mouse button allowed a vertical crossing");
}

static void test_jump_threshold_uses_the_vertical_seam_axis(void) {
    device_t state = stacked_computers_state();
    state.pointer_y = 10;
    global_state = state;
    mouse_values_t movement = {.move_y = -15};

    CHECK(update_mouse_position(&state, &movement) == NONE,
          "vertical crossing ignored the configured jump threshold");

    state.pointer_y = 10;
    movement.move_y = -16;
    CHECK(update_mouse_position(&state, &movement) == TOP,
          "vertical crossing did not trigger beyond the jump threshold");
}

static void test_configured_seam_maps_position_and_blocks_gaps(void) {
    device_t state = stacked_computers_state();
    state.pointer_x = MAX_SCREEN_COORD / 2;
    state.config.output[0].seam_ranges[0] = (dh_seam_range_t){
        .screen_index = 2, .start = 0, .end = DH_SEAM_POSITION_MAX,
    };
    state.config.output[1].seam_ranges[0] = (dh_seam_range_t){
        .screen_index = 1, .start = 16384, .end = 32768,
    };

    output_switches = 0;
    do_screen_switch(&state, TOP);
    CHECK(output_switches == 1, "configured seam range did not cross");
    CHECK(state.pointer_x == 12288,
          "crossing position was not scaled onto the target range");
    CHECK(state.config.output[1].screen_index == 1,
          "crossing did not select the target range's screen index");

    state = stacked_computers_state();
    state.config.output[0].seam_ranges[0] = (dh_seam_range_t){
        .screen_index = 1, .start = 0, .end = DH_SEAM_POSITION_MAX,
    };
    state.config.output[1].seam_ranges[0] = (dh_seam_range_t){
        .screen_index = 1, .start = 0, .end = DH_SEAM_POSITION_MAX,
    };
    output_switches = 0;
    do_screen_switch(&state, TOP);
    CHECK(output_switches == 0, "monitor without a configured range crossed");
}

static void test_half_configured_seam_keeps_legacy_crossing(void) {
    device_t state = stacked_computers_state();
    state.config.output[0].seam_ranges[0] = (dh_seam_range_t){
        .screen_index = 2, .start = 0, .end = DH_SEAM_POSITION_MAX,
    };

    output_switches = 0;
    do_screen_switch(&state, TOP);
    CHECK(output_switches == 1,
          "half-configured seam trapped the cursor on the source output");
}

static void test_public_mouse_seam_matches_mkroamer_edge_map(void) {
    const int shared_pointer_positions[] = {500, 4096, 8192, 12000, 15000};
    for (unsigned i = 0; i < sizeof shared_pointer_positions / sizeof shared_pointer_positions[0];
         i++) {
        device_t state = stacked_computers_state();
        state.pointer_x = (int16_t)shared_pointer_positions[i];
        state.config.output[0].seam_ranges[0] = (dh_seam_range_t){
            .screen_index = 2, .start = 1000, .end = 32000,
        };
        state.config.output[1].seam_ranges[0] = (dh_seam_range_t){
            .screen_index = 3, .start = 12000, .end = 60000,
        };

        const uint16_t source = (uint16_t)(
            ((uint32_t)state.pointer_x * DH_SEAM_POSITION_MAX + MAX_SCREEN_COORD / 2) /
            MAX_SCREEN_COORD);
        const uint16_t expected_normalized =
            mkroamer_entry(mkroamer_cross(source, 1000, 32000), 12000, 60000);
        const int expected_pointer = (int)(
            ((uint32_t)expected_normalized * MAX_SCREEN_COORD + DH_SEAM_POSITION_MAX / 2) /
            DH_SEAM_POSITION_MAX);

        output_switches = 0;
        do_screen_switch(&state, TOP);
        CHECK(output_switches == 1, "mkroamer shared input did not cross the mouse seam");
        CHECK(state.pointer_x == expected_pointer,
              "public mouse seam differs from mkroamer's edge map");
        CHECK(state.config.output[1].screen_index == 3,
              "public mouse seam lost mkroamer's segment identity");
    }
}

static void test_invalid_target_screen_degrades_to_legacy_crossing(void) {
    device_t state = stacked_computers_state();
    state.config.output[0].seam_ranges[0] = (dh_seam_range_t){
        .screen_index = 2, .start = 0, .end = DH_SEAM_POSITION_MAX,
    };
    state.config.output[1].screen_count = 2;
    state.config.output[1].screen_index = 1;
    state.config.output[1].seam_ranges[0] = (dh_seam_range_t){
        .screen_index = 255, .start = 0, .end = DH_SEAM_POSITION_MAX,
    };

    output_switches = 0;
    do_screen_switch(&state, TOP);
    CHECK(output_switches == 1, "invalid target screen trapped the cursor");
    CHECK(state.config.output[1].screen_index == 1,
          "invalid seam range selected a nonexistent target monitor");
}

static void test_update_and_switch_at_the_public_mouse_seam(void) {
    device_t state = side_by_side_state();
    global_state = state;
    mouse_values_t movement = {.move_x = -16};

    enum screen_pos_e direction = update_mouse_position(&state, &movement);
    CHECK(direction == LEFT, "update_mouse_position did not detect the thresholded border edge");
    CHECK(state.pointer_x == MIN_SCREEN_COORD, "update_mouse_position did not clamp the pointer");

    output_switches = 0;
    do_screen_switch(&state, direction);
    CHECK(output_switches == 1 && state.active_output == 1,
          "do_screen_switch did not cross from the border-adjacent monitor");
}

static void test_virtual_desktops_remain_local(void) {
    device_t state = side_by_side_state();
    output_switches = 0;
    virtual_switches = 0;
    do_screen_switch(&state, RIGHT);
    CHECK(virtual_switches == 1 && requested_index == 2,
          "do_screen_switch did not advance along the monitor chain");

    do_screen_switch(&state, LEFT);
    CHECK(virtual_switches == 2 && requested_index == 1 && output_switches == 0,
          "do_screen_switch did not return locally to the border-adjacent monitor");
}

int main(void) {
    test_update_and_switch_at_the_public_mouse_seam();
    test_virtual_desktops_remain_local();
    test_perpendicular_seam_crosses_from_any_monitor();
    test_diagonal_push_uses_the_larger_overshoot();
    test_vertical_seam_preserves_crossing_guards();
    test_jump_threshold_uses_the_vertical_seam_axis();
    test_configured_seam_maps_position_and_blocks_gaps();
    test_half_configured_seam_keeps_legacy_crossing();
    test_public_mouse_seam_matches_mkroamer_edge_map();
    test_invalid_target_screen_degrades_to_legacy_crossing();
    if (failures) return 1;
    printf("mouse_seam_test: all checks passed\n");
    return 0;
}
