#include <stdio.h>
#include <string.h>

#include "main.h"

device_t global_state;
static int output_switches;
static int virtual_switches;
static int requested_index;
static int placements;
static uint8_t placed_output;
static uint8_t placed_screen;
static uint8_t placed_chain;
static uint8_t placed_border;
static uint16_t placed_position;
static int source_queries;
static bool source_query_available;
static uint8_t source_query_id;

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

void channel_place_cursor(uint8_t output, uint8_t screen, uint8_t chain, uint8_t border,
                          uint16_t position) {
    placements++;
    placed_output = output;
    placed_screen = screen;
    placed_chain = chain;
    placed_border = border;
    placed_position = position;
}

bool channel_query_cursor(uint8_t output, uint8_t query_id) {
    (void)output;
    source_queries++;
    source_query_id = query_id;
    return source_query_available;
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

static void test_mapped_crossing_places_cursor_on_target_monitor(void) {
    device_t state = stacked_computers_state();
    state.pointer_x = MAX_SCREEN_COORD / 2;
    state.config.output[0].screen_index = 1;
    state.config.output[0].border_direction = DH_DIRECTION_BOTTOM;
    state.config.output[1].border_direction = DH_DIRECTION_TOP;
    state.config.output[1].os = WINDOWS;
    state.config.output[0].seam_ranges[0] = (dh_seam_range_t){
        .screen_index = 2, .start = 0, .end = DH_SEAM_POSITION_MAX,
    };
    state.config.output[1].seam_ranges[0] = (dh_seam_range_t){
        .screen_index = 1, .start = 0, .end = DH_SEAM_POSITION_MAX,
    };
    state.config.output[0].seam_ranges[1] = (dh_seam_range_t){
        .screen_index = 1, .start = 0, .end = DH_SEAM_POSITION_MAX,
    };
    state.config.output[1].seam_ranges[1] = (dh_seam_range_t){
        .screen_index = 2, .start = 0, .end = DH_SEAM_POSITION_MAX,
    };

    placements = 0;
    output_switches = 0;
    do_screen_switch(&state, BOTTOM);

    CHECK(output_switches == 1, "right-pair seam did not cross to the other output");
    CHECK(placements == 1, "mapped crossing did not request target-helper placement");
    CHECK(placed_output == 1 && placed_screen == 2,
          "mapped crossing requested placement on the wrong target monitor");
    CHECK(placed_chain == DH_DIRECTION_LEFT,
          "mapped crossing did not describe the target monitor chain");
    CHECK(placed_border == DH_DIRECTION_TOP,
          "mapped crossing did not describe the target output's entry edge");
    CHECK(placed_position == 32766,
          "mapped crossing did not preserve the normalized seam position");
    CHECK(state.relative_mouse,
          "mapped crossing left target screen 2 absolute while placement was pending");
}

static void test_relative_source_crossing_waits_for_true_position(void) {
    device_t state = stacked_computers_state();
    state.active_output = 0;
    state.config.output[0].os = WINDOWS;
    state.config.output[0].screen_index = 2;
    state.config.output[0].border_direction = DH_DIRECTION_TOP;
    state.config.output[0].seam_ranges[0] = (dh_seam_range_t){
        .screen_index = 2, .start = 0, .end = DH_SEAM_POSITION_MAX,
    };
    state.config.output[1].seam_ranges[0] = (dh_seam_range_t){
        .screen_index = 1, .start = 0, .end = DH_SEAM_POSITION_MAX,
    };
    state.relative_mouse = true;
    state.pointer_x = 4000; /* stale firmware estimate */
    state.pointer_y = MIN_SCREEN_COORD;
    global_state = state;
    source_query_available = true;
    source_queries = 0;
    output_switches = 0;
    placements = 0;

    do_screen_switch(&state, TOP);
    CHECK(output_switches == 0 && state.cursor_crossing.phase == CURSOR_CROSSING_WAITING,
          "relative source crossed before its helper re-anchored it");
    CHECK(source_queries == 1, "relative source crossing did not query its helper once");

    CHECK(apply_helper_cursor_position(&state, 0, 2, 20000, MIN_SCREEN_COORD,
                                       source_query_id),
          "true source position was refused");
    mouse_crossing_task(&state, 1000);
    CHECK(output_switches == 1 && placements == 1,
          "source response did not resume exactly one crossing");
    CHECK(placed_position >= 39999 && placed_position <= 40001,
          "resumed crossing did not use the helper's true source coordinate");

    mouse_crossing_task(&state, 1001);
    CHECK(output_switches == 1, "source response triggered a duplicate crossing");
}

static void test_relative_source_crossing_falls_back_without_helper(void) {
    device_t state = stacked_computers_state();
    state.active_output = 0;
    state.config.output[0].os = WINDOWS;
    state.config.output[0].screen_index = 2;
    state.config.output[0].border_direction = DH_DIRECTION_TOP;
    state.relative_mouse = true;
    source_query_available = false;
    output_switches = 0;

    do_screen_switch(&state, TOP);
    CHECK(output_switches == 1 && state.cursor_crossing.phase == CURSOR_CROSSING_IDLE,
          "missing source helper stalled the firmware-only crossing");
}

static device_t pending_relative_crossing(void) {
    device_t state = stacked_computers_state();
    state.active_output = 0;
    state.config.output[0].os = WINDOWS;
    state.config.output[0].screen_index = 2;
    state.config.output[0].border_direction = DH_DIRECTION_TOP;
    state.relative_mouse = true;
    source_query_available = true;
    output_switches = 0;
    do_screen_switch(&state, TOP);
    return state;
}

static void test_relative_source_crossing_falls_back_on_unavailable_reply(void) {
    device_t state = pending_relative_crossing();
    CHECK(state.cursor_crossing.phase == CURSOR_CROSSING_WAITING && output_switches == 0,
          "unavailable test did not begin a pending crossing");
    mouse_crossing_query_unavailable(&state, 0, state.cursor_crossing.query_id);
    mouse_crossing_task(&state, 1);
    CHECK(output_switches == 1 && state.cursor_crossing.phase == CURSOR_CROSSING_IDLE,
          "unavailable source helper did not resume the fallback crossing");
}

static void test_relative_source_crossing_falls_back_on_timeout(void) {
    device_t state = pending_relative_crossing();
    CHECK(state.cursor_crossing.phase == CURSOR_CROSSING_WAITING && output_switches == 0,
          "timeout test did not begin a pending crossing");
    mouse_crossing_task(&state, 29999);
    CHECK(output_switches == 0, "source query timed out before its bounded deadline");
    mouse_crossing_task(&state, 30000);
    CHECK(output_switches == 1 && state.cursor_crossing.phase == CURSOR_CROSSING_IDLE,
          "source query timeout trapped the cursor at the seam");
}

static void test_source_response_away_from_edge_cancels_crossing(void) {
    device_t state = pending_relative_crossing();
    CHECK(apply_helper_cursor_position(&state, 0, 2, 20000, 5000,
                                       state.cursor_crossing.query_id),
          "away-from-edge source position was refused");
    mouse_crossing_task(&state, 1);
    CHECK(output_switches == 0 && state.cursor_crossing.phase == CURSOR_CROSSING_IDLE,
          "source response after reversing away still crossed outputs");
}

static void test_late_source_response_cannot_satisfy_a_new_crossing(void) {
    device_t state = pending_relative_crossing();
    const uint8_t first_query_id = state.cursor_crossing.query_id;
    mouse_crossing_task(&state, 30000);
    CHECK(output_switches == 1, "first crossing did not take its timeout fallback");

    state.active_output = 0;
    state.config.output[0].screen_index = 2;
    state.relative_mouse = true;
    state.pointer_y = MIN_SCREEN_COORD;
    do_screen_switch(&state, TOP);
    const uint8_t second_query_id = state.cursor_crossing.query_id;
    CHECK(second_query_id != first_query_id &&
              state.cursor_crossing.phase == CURSOR_CROSSING_WAITING,
          "new crossing did not receive a distinct query id");

    CHECK(!apply_helper_cursor_position(&state, 0, 2, 5000, MIN_SCREEN_COORD,
                                        first_query_id),
          "late response from the first query was accepted by the second");
    CHECK(state.cursor_crossing.phase == CURSOR_CROSSING_WAITING && state.pointer_x != 5000,
          "late response changed the pending crossing state");
    mouse_crossing_task(&state, 1);
    CHECK(output_switches == 1, "late response triggered a second crossing");

    CHECK(apply_helper_cursor_position(&state, 0, 2, 20000, MIN_SCREEN_COORD,
                                       second_query_id),
          "matching response for the second query was refused");
    mouse_crossing_task(&state, 2);
    CHECK(output_switches == 2, "matching response did not resume the second crossing");
}

static void test_post_placement_refresh_cannot_satisfy_a_source_query(void) {
    device_t state = pending_relative_crossing();
    state.pointer_x = 4000;
    CHECK(!apply_helper_cursor_position(&state, 0, 2, 20000, MIN_SCREEN_COORD, 0),
          "uncorrelated post-placement refresh was accepted during a source query");
    CHECK(state.cursor_crossing.phase == CURSOR_CROSSING_WAITING && state.pointer_x == 4000,
          "post-placement refresh changed a pending source crossing");
    mouse_crossing_task(&state, 1);
    CHECK(output_switches == 0, "post-placement refresh resumed a source crossing");
}

static void test_pending_crossing_cancels_after_output_change(void) {
    device_t state = pending_relative_crossing();
    state.active_output = 1;
    mouse_crossing_task(&state, 30000);
    CHECK(output_switches == 0 && state.cursor_crossing.phase == CURSOR_CROSSING_IDLE,
          "timed-out query crossed a different active output");
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
    test_mapped_crossing_places_cursor_on_target_monitor();
    test_relative_source_crossing_waits_for_true_position();
    test_relative_source_crossing_falls_back_without_helper();
    test_relative_source_crossing_falls_back_on_unavailable_reply();
    test_relative_source_crossing_falls_back_on_timeout();
    test_source_response_away_from_edge_cancels_crossing();
    test_late_source_response_cannot_satisfy_a_new_crossing();
    test_post_placement_refresh_cannot_satisfy_a_source_query();
    test_pending_crossing_cancels_after_output_change();
    test_half_configured_seam_keeps_legacy_crossing();
    test_public_mouse_seam_matches_mkroamer_edge_map();
    test_invalid_target_screen_degrades_to_legacy_crossing();
    if (failures) return 1;
    printf("mouse_seam_test: all checks passed\n");
    return 0;
}
