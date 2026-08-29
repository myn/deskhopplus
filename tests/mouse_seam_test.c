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
    if (failures) return 1;
    printf("mouse_seam_test: all checks passed\n");
    return 0;
}
