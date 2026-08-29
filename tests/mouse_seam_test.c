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
    if (failures) return 1;
    printf("mouse_seam_test: all checks passed\n");
    return 0;
}
