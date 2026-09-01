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
static int source_query_retries;
static uint8_t source_query_id;
static bool placement_query_available;
static uint8_t placement_query_id;

enum screen_pos_e update_mouse_position(device_t *, mouse_values_t *);
void do_screen_switch(device_t *, int);

void switch_to_another_pc(device_t *state, output_t *output, int output_to, int direction) {
    (void)output;
    output_switches++;
    state->active_output = (uint8_t)output_to;
    const dh_mouse_coordinates_t entry = dh_mouse_entry_coordinates(
        (dh_direction_t)direction,
        (dh_mouse_coordinates_t){.x = state->pointer_x, .y = state->pointer_y},
        MIN_SCREEN_COORD, MAX_SCREEN_COORD);
    state->pointer_x = (int16_t)entry.x;
    state->pointer_y = (int16_t)entry.y;
}

void switch_virtual_desktop(device_t *state, output_t *output, int new_index, int direction) {
    virtual_switches++;
    requested_index = new_index;
    const dh_mouse_coordinates_t entry = dh_mouse_entry_coordinates(
        (dh_direction_t)direction,
        (dh_mouse_coordinates_t){.x = state->pointer_x, .y = state->pointer_y},
        MIN_SCREEN_COORD, MAX_SCREEN_COORD);
    state->pointer_x = (int16_t)entry.x;
    state->pointer_y = (int16_t)entry.y;
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

cursor_query_result_t channel_query_cursor(uint8_t output, uint8_t query_id) {
    (void)output;
    source_queries++;
    source_query_id = query_id;
    if (source_query_retries > 0) {
        source_query_retries--;
        return CURSOR_QUERY_RETRY;
    }
    return source_query_available ? CURSOR_QUERY_SENT : CURSOR_QUERY_UNAVAILABLE;
}

bool channel_place_cursor_correlated(uint8_t output, uint8_t screen, uint8_t chain,
                                     uint8_t border, uint16_t position, uint8_t query_id) {
    channel_place_cursor(output, screen, chain, border, position);
    placement_query_id = query_id;
    return placement_query_available;
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

static device_t four_screen_corner_state(enum screen_pos_e horizontal,
                                         enum screen_pos_e vertical) {
    device_t state = side_by_side_state();
    state.active_output = 0;
    state.config.output[0].chain_direction = DH_DIRECTION_RIGHT;
    state.config.output[1].chain_direction = DH_DIRECTION_LEFT;
    state.config.output[0].border_direction = (uint8_t)vertical;
    state.config.output[1].border_direction = (uint8_t)dh_opposite_direction(
        (dh_direction_t)vertical);
    state.config.output[0].screen_count = 2;
    state.config.output[1].screen_count = 2;
    const uint8_t source_screen = horizontal == RIGHT ? 1 : 2;
    state.config.output[0].screen_index = source_screen;
    state.config.output[1].screen_index = source_screen == 1 ? 2 : 1;
    state.config.output[0].os = WINDOWS;
    state.config.output[1].os = MACOS;
    state.relative_mouse = source_screen > 1;
    for (uint8_t screen = 1; screen <= 2; screen++) {
        state.config.output[0].seam_ranges[screen - 1] = (dh_seam_range_t){
            .screen_index = screen, .start = 0, .end = DH_SEAM_POSITION_MAX,
        };
        state.config.output[1].seam_ranges[screen - 1] = (dh_seam_range_t){
            .screen_index = screen == 1 ? 2 : 1,
            .start = 0, .end = DH_SEAM_POSITION_MAX,
        };
    }
    return state;
}

typedef struct {
    int16_t x;
    int16_t y;
    int32_t move_x;
    int32_t move_y;
    enum screen_pos_e horizontal;
    enum screen_pos_e vertical;
} corner_case_t;

static const corner_case_t corner_cases[] = {
    {2, 2, -10, -20, LEFT, TOP},
    {MAX_SCREEN_COORD - 2, 2, 10, -20, RIGHT, TOP},
    {2, MAX_SCREEN_COORD - 2, -10, 20, LEFT, BOTTOM},
    {MAX_SCREEN_COORD - 2, MAX_SCREEN_COORD - 2, 10, 20, RIGHT, BOTTOM},
};

static void test_perpendicular_seam_crosses_from_any_monitor(void) {
    device_t state = stacked_computers_state();
    global_state = state;
    mouse_values_t movement = {.move_y = -16};

    enum screen_pos_e direction = update_mouse_position(&state, &movement);
    CHECK(direction == TOP, "update_mouse_position did not detect the top seam");
    CHECK(state.pointer_y == 10,
          "update_mouse_position discarded the last valid Y before switching");

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
    CHECK(state.pointer_x == 2 && state.pointer_y == 2,
          "diagonal push manufactured an exact corner before switching");

    state.pointer_x = 2;
    state.pointer_y = 2;
    movement.move_x = -12;
    movement.move_y = -14;
    CHECK(update_mouse_position(&state, &movement) == TOP,
          "jump threshold distorted the corner overshoot comparison");
}

static void test_non_transition_and_guarded_edges_clamp_normally(void) {
    device_t state = side_by_side_state();
    state.pointer_y = 10;
    global_state = state;
    mouse_values_t movement = {.move_y = -11};
    CHECK(update_mouse_position(&state, &movement) == NONE,
          "a physical edge with no transition requested a screen switch");
    CHECK(state.pointer_y == MIN_SCREEN_COORD,
          "a physical edge with no transition froze short of the boundary");

    state = stacked_computers_state();
    state.switch_lock = true;
    global_state = state;
    movement = (mouse_values_t){.move_y = -16};
    CHECK(update_mouse_position(&state, &movement) == NONE,
          "switch lock left a pending geometric crossing");
    CHECK(state.pointer_y == MIN_SCREEN_COORD,
          "switch lock froze the cursor short of the output edge");

    state = stacked_computers_state();
    global_state = state;
    movement = (mouse_values_t){.move_y = -16, .buttons = 1};
    CHECK(update_mouse_position(&state, &movement) == NONE,
          "held button left a blocked output crossing pending");
    CHECK(state.pointer_y == MIN_SCREEN_COORD,
          "held button froze the cursor short of the output edge");
}

static void test_diagonal_corner_continues_without_a_synthetic_adjacent_seam(void) {
    for (unsigned i = 0; i < sizeof corner_cases / sizeof corner_cases[0]; i++) {
        const corner_case_t *corner = &corner_cases[i];
        const uint8_t source_screen = corner->horizontal == RIGHT ? 1 : 2;
        device_t state = four_screen_corner_state(corner->horizontal, corner->vertical);
        state.pointer_x = corner->x;
        state.pointer_y = corner->y;
        global_state = state;
        mouse_values_t movement = {
            .move_x = corner->move_x,
            .move_y = corner->move_y,
        };

        CHECK(update_mouse_position(&state, &movement) == corner->vertical,
              "vertical-first corner did not choose the farther overshoot");
        CHECK(state.pointer_x == corner->x && state.pointer_y == corner->y,
              "vertical-first corner discarded its last valid coordinates");
        CHECK(movement.move_x == (source_screen > 1 ? 0 : corner->move_x) &&
                  movement.move_y == corner->move_y,
              "vertical-first corner sent relative motion into the losing seam");

        do_screen_switch(&state, corner->vertical);
        CHECK(state.active_output == 1,
              "vertical-first corner did not cross to the target output");
        const uint8_t paired_target_screen = source_screen == 1 ? 2 : 1;
        CHECK(state.config.output[1].screen_index == paired_target_screen,
              "vertical-first corner selected the wrong paired monitor");
        movement.move_x = corner->move_x;
        movement.move_y = corner->move_y;
        CHECK(update_mouse_position(&state, &movement) == corner->horizontal,
              "continued diagonal did not reach the intended adjacent monitor");
        CHECK(state.pointer_y != MIN_SCREEN_COORD && state.pointer_y != MAX_SCREEN_COORD,
              "continued diagonal snapped its inward coordinate to a corner");
        do_screen_switch(&state, corner->horizontal);
        CHECK(state.active_output == 1 &&
                  state.config.output[1].screen_index == source_screen,
              "continued diagonal did not enter the adjacent target monitor");

        state = four_screen_corner_state(corner->horizontal, corner->vertical);
        state.pointer_x = corner->x;
        state.pointer_y = corner->y;
        global_state = state;
        CHECK(update_mouse_position(&state, &movement) == corner->vertical,
              "reversal setup did not choose the vertical seam");
        do_screen_switch(&state, corner->vertical);
        mouse_values_t reversal = {
            .move_x = -corner->move_x,
            .move_y = -corner->move_y,
        };
        const enum screen_pos_e reverse_vertical =
            corner->vertical == TOP ? BOTTOM : TOP;
        CHECK(update_mouse_position(&state, &reversal) == reverse_vertical,
              "reversing after a vertical crossing triggered the unchosen seam");
        do_screen_switch(&state, reverse_vertical);
        CHECK(state.active_output == 0 &&
                  state.config.output[0].screen_index == source_screen,
              "reversal did not return through the paired vertical seam");

        state = four_screen_corner_state(corner->horizontal, corner->vertical);
        state.pointer_x = corner->x;
        state.pointer_y = corner->y;
        movement.move_x = corner->move_x * 2;
        movement.move_y = corner->move_y / 2;
        global_state = state;
        CHECK(update_mouse_position(&state, &movement) == corner->horizontal,
              "horizontal-first corner did not choose the farther overshoot");
        CHECK(state.pointer_x == corner->x && state.pointer_y == corner->y,
              "horizontal-first corner discarded its last valid coordinates");
        CHECK(movement.move_x == corner->move_x * 2 &&
                  movement.move_y == (source_screen > 1 ? 0 : corner->move_y / 2),
              "horizontal-first corner sent relative motion into the losing seam");

        do_screen_switch(&state, corner->horizontal);
        CHECK(state.active_output == 0 &&
                  state.config.output[0].screen_index == (source_screen == 1 ? 2 : 1),
              "horizontal-first corner did not enter the adjacent source monitor");
        movement.move_x = corner->move_x * 2;
        movement.move_y = corner->move_y / 2;
        CHECK(update_mouse_position(&state, &movement) == corner->vertical,
              "horizontal-first continuation did not reach the intended adjacent seam");
        CHECK(state.pointer_x != MIN_SCREEN_COORD && state.pointer_x != MAX_SCREEN_COORD,
              "horizontal-first continuation snapped its inward coordinate to a corner");
        do_screen_switch(&state, corner->vertical);
        CHECK(state.active_output == 1,
              "horizontal-first continuation did not cross to the target output");
    }
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

    state.pointer_y = 10;
    global_state = state;
    mouse_values_t movement = {.move_y = -16};
    const enum screen_pos_e direction = update_mouse_position(&state, &movement);
    CHECK(direction == TOP && state.pointer_y == 10,
          "blocked seam setup did not preserve the last valid crossing coordinate");
    do_screen_switch(&state, direction);
    CHECK(state.pointer_y == MIN_SCREEN_COORD,
          "blocked seam left the cursor frozen short of its physical edge");
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

static void test_fast_diagonal_cannot_change_screens_while_source_query_is_pending(void) {
    for (unsigned i = 0; i < sizeof corner_cases / sizeof corner_cases[0]; i++) {
        const corner_case_t *corner = &corner_cases[i];
        device_t state = four_screen_corner_state(corner->horizontal, corner->vertical);
        state.pointer_x = corner->x;
        state.pointer_y = corner->y;
        state.config.output[0].screen_index = 2;
        state.config.output[0].chain_direction = (uint8_t)dh_opposite_direction(
            (dh_direction_t)corner->horizontal);
        state.config.output[1].chain_direction = DH_DIRECTION_RIGHT;
        state.relative_mouse = true;
        memset(state.config.output[0].seam_ranges, 0,
               sizeof state.config.output[0].seam_ranges);
        memset(state.config.output[1].seam_ranges, 0,
               sizeof state.config.output[1].seam_ranges);
        const uint8_t target_screen = corner->horizontal == LEFT ? 2 : 1;
        state.config.output[0].seam_ranges[0] = (dh_seam_range_t){
            .screen_index = 2, .start = 0, .end = DH_SEAM_POSITION_MAX,
        };
        state.config.output[1].seam_ranges[0] = (dh_seam_range_t){
            .screen_index = target_screen, .start = 0, .end = DH_SEAM_POSITION_MAX,
        };
        global_state = state;
        source_query_available = true;
        source_queries = 0;
        output_switches = 0;
        virtual_switches = 0;
        placements = 0;

        mouse_values_t movement = {
            .move_x = corner->move_x,
            .move_y = corner->move_y,
        };
        const enum screen_pos_e first = update_mouse_position(&state, &movement);
        CHECK(first == corner->vertical,
              "fast-diagonal setup did not choose the output seam");
        do_screen_switch(&state, first);
        CHECK(state.cursor_crossing.phase == CURSOR_CROSSING_WAITING && source_queries == 1,
              "fast-diagonal setup did not leave a pending source query");

        movement = (mouse_values_t){
            .move_x = corner->move_x * 2,
            .move_y = corner->move_y / 2,
        };
        const enum screen_pos_e during_query = update_mouse_position(&state, &movement);
        if (during_query != NONE)
            do_screen_switch(&state, during_query);
        CHECK(during_query == NONE,
              "a fast follow-up packet selected another seam during source re-anchor");
        CHECK(virtual_switches == 0 && state.config.output[0].screen_index == 2,
              "a fast follow-up packet changed the Windows monitor during source re-anchor");
        CHECK(movement.move_x == 0 && movement.move_y == 0,
              "a fast follow-up packet moved the OS cursor during source re-anchor");

        CHECK(apply_helper_cursor_position(&state, 0, 2, corner->x, corner->y,
                                           state.cursor_crossing.query_id),
              "fast-diagonal source position was refused");
        mouse_crossing_task(&state, 1);
        CHECK(output_switches == 1 && placements == 1 && state.active_output == 1,
              "fast-diagonal source response did not resume the mapped output crossing");
        CHECK(state.config.output[1].screen_index == target_screen,
              "fast-diagonal output crossing selected the wrong target monitor");

        movement = (mouse_values_t){
            .move_x = corner->move_x,
            .move_y = corner->move_y,
        };
        const enum screen_pos_e continuation = update_mouse_position(&state, &movement);
        CHECK(continuation == corner->horizontal,
              "fast diagonal did not continue through the intended adjacent seam");
        CHECK(state.pointer_y != MIN_SCREEN_COORD && state.pointer_y != MAX_SCREEN_COORD,
              "fast diagonal resumed at a synthetic target corner");
        do_screen_switch(&state, continuation);
        CHECK(state.config.output[1].screen_index == (target_screen == 1 ? 2 : 1),
              "fast diagonal did not reach the adjacent target monitor");
    }
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

static void test_stranded_resuming_crossing_releases_mouse_input(void) {
    device_t state = pending_relative_crossing();
    /* Reproduce the captured failure: the pending source query times out, but
       the transition is no longer actionable when fallback tries to resume
       it. The old code let RESUMING escape this call indefinitely. */
    state.config.output[0].border_direction = DH_DIRECTION_LEFT;

    mouse_crossing_task(&state, 30000);
    CHECK(state.cursor_crossing.phase == CURSOR_CROSSING_IDLE,
          "a stranded RESUMING crossing kept suppressing mouse input");

    state.pointer_x = 16000;
    state.pointer_y = 16000;
    mouse_values_t movement = {.move_x = 3, .move_y = 2};
    update_mouse_position(&state, &movement);
    CHECK(movement.move_x == 3 && movement.move_y == 2,
          "the first input after RESUMING recovery was swallowed");
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
    CHECK(state.pointer_x == 10,
          "update_mouse_position discarded the last valid X before switching");

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

static void test_confirmed_macos_chain_forward_uses_only_helper_placement(void) {
    device_t state = side_by_side_state();
    state.active_output = 0;
    state.config.output[0].os = MACOS;
    state.config.output[0].screen_index = 1;
    state.pointer_x = MAX_SCREEN_COORD;
    state.pointer_y = 12345;
    placement_query_available = true;
    placements = 0;
    virtual_switches = 0;

    do_screen_switch(&state, RIGHT);

    CHECK(placements == 1 && state.cursor_crossing.phase == CURSOR_CROSSING_WAITING,
          "macOS chain-forward did not wait for correlated helper placement");
    CHECK(virtual_switches == 0 && state.config.output[0].screen_index == 1,
          "macOS chain-forward emitted the legacy transition before confirmation");
    CHECK(apply_helper_cursor_position(&state, 0, 2, MIN_SCREEN_COORD, 12345,
                                       placement_query_id),
          "macOS chain-forward placement confirmation was refused");
    mouse_crossing_task(&state, 1);
    CHECK(virtual_switches == 0 && state.config.output[0].screen_index == 2 &&
              state.cursor_crossing.phase == CURSOR_CROSSING_IDLE,
          "confirmed macOS chain-forward emitted legacy edge/nudge reports");
    CHECK(!apply_helper_cursor_position(&state, 0, 1, MAX_SCREEN_COORD, 999,
                                        placement_query_id) &&
              state.config.output[0].screen_index == 2 && state.pointer_y == 12345,
          "duplicate Mac placement response mutated the completed crossing");
    placement_query_available = false;
}

static void test_confirmed_macos_chain_back_uses_only_helper_placement(void) {
    device_t state = side_by_side_state();
    state.active_output = 0;
    state.config.output[0].os = MACOS;
    state.config.output[0].screen_index = 2;
    state.pointer_x = MIN_SCREEN_COORD;
    state.pointer_y = 23456;
    placement_query_available = true;
    placements = 0;
    virtual_switches = 0;

    do_screen_switch(&state, LEFT);
    CHECK(placements == 1 && placed_screen == 1 && placed_border == RIGHT &&
              placed_position > 46911 && placed_position < 46914,
          "macOS chain-back did not preserve the destination-edge coordinate");
    CHECK(apply_helper_cursor_position(&state, 0, 1, MAX_SCREEN_COORD, 23456,
                                       placement_query_id),
          "macOS chain-back placement confirmation was refused");
    mouse_crossing_task(&state, 1);
    CHECK(virtual_switches == 0 && state.config.output[0].screen_index == 1,
          "confirmed macOS chain-back emitted the legacy transition");
    placement_query_available = false;
}

static void test_macos_chain_fallbacks_once(void) {
    device_t state = side_by_side_state();
    state.config.output[0].os = MACOS;
    state.config.output[0].screen_index = 1;
    placement_query_available = false;
    virtual_switches = 0;
    do_screen_switch(&state, RIGHT);
    CHECK(virtual_switches == 1 && state.config.output[0].screen_index == 2,
          "unavailable Mac helper did not run the legacy fallback exactly once");

    state = side_by_side_state();
    state.config.output[0].os = MACOS;
    state.config.output[0].screen_index = 1;
    placement_query_available = true;
    virtual_switches = 0;
    do_screen_switch(&state, RIGHT);
    const uint8_t query_id = state.cursor_crossing.query_id;
    mouse_crossing_query_unavailable(&state, 0, query_id);
    mouse_crossing_task(&state, 1);
    mouse_crossing_task(&state, 2);
    CHECK(virtual_switches == 1 && state.config.output[0].screen_index == 2,
          "refused Mac placement did not run the legacy fallback exactly once");

    state = side_by_side_state();
    state.config.output[0].os = MACOS;
    state.config.output[0].screen_index = 1;
    virtual_switches = 0;
    do_screen_switch(&state, RIGHT);
    const uint8_t timed_out_id = state.cursor_crossing.query_id;
    mouse_crossing_task(&state, 30000);
    mouse_crossing_task(&state, 30001);
    CHECK(virtual_switches == 1 && state.config.output[0].screen_index == 2,
          "timed-out Mac placement did not run the legacy fallback exactly once");
    CHECK(!apply_helper_cursor_position(&state, 0, 2, MIN_SCREEN_COORD, 100,
                                        timed_out_id) && virtual_switches == 1,
          "late Mac placement response was accepted after fallback");
    placement_query_available = false;
}

static void test_macos_chain_holds_only_position_while_pending(void) {
    device_t state = side_by_side_state();
    state.config.output[0].os = MACOS;
    placement_query_available = true;
    do_screen_switch(&state, RIGHT);
    mouse_values_t movement = {
        .move_x = 40, .move_y = -30, .buttons = 1, .wheel = 2, .pan = -3,
    };
    CHECK(update_mouse_position(&state, &movement) == NONE &&
              movement.move_x == 0 && movement.move_y == 0,
          "pending Mac placement did not hold positional input");
    CHECK(movement.buttons == 1 && movement.wheel == 2 && movement.pan == -3 &&
              state.mouse_buttons == 1,
          "pending Mac placement did not preserve buttons, wheel, and pan");
    placement_query_available = false;
}

static void test_macos_chain_requires_the_requested_placement_coordinate(void) {
    device_t state = side_by_side_state();
    state.config.output[0].os = MACOS;
    state.pointer_y = 12000;
    placement_query_available = true;
    virtual_switches = 0;
    do_screen_switch(&state, RIGHT);
    CHECK(!apply_helper_cursor_position(&state, 0, 2, MIN_SCREEN_COORD, 20000,
                                        placement_query_id) &&
              state.cursor_crossing.phase == CURSOR_CROSSING_WAITING,
          "target-screen response falsely confirmed a refused Mac placement");
    mouse_crossing_task(&state, 30000);
    CHECK(virtual_switches == 1,
          "unconfirmed Mac placement did not reach the bounded legacy fallback");
    placement_query_available = false;
}

static void test_fast_diagonal_macos_chain_uses_correlated_placement(void) {
    for (unsigned i = 0; i < sizeof corner_cases / sizeof corner_cases[0]; i++) {
        const corner_case_t *corner = &corner_cases[i];
        device_t state = four_screen_corner_state(corner->horizontal, corner->vertical);
        state.pointer_x = corner->x;
        state.pointer_y = corner->y;
        global_state = state;
        output_switches = 0;
        virtual_switches = 0;
        placements = 0;
        placement_query_available = false;
        mouse_values_t movement = {.move_x = corner->move_x, .move_y = corner->move_y};
        const enum screen_pos_e output_seam = update_mouse_position(&state, &movement);
        do_screen_switch(&state, output_seam);

        placement_query_available = true;
        movement = (mouse_values_t){.move_x = corner->move_x, .move_y = corner->move_y};
        const enum screen_pos_e chain_seam = update_mouse_position(&state, &movement);
        const int16_t preserved_along_edge = state.pointer_y;
        do_screen_switch(&state, chain_seam);
        const uint8_t expected_screen = state.config.output[1].screen_index == 1 ? 2 : 1;
        const uint16_t expected_position = (uint16_t)(
            ((uint32_t)preserved_along_edge * DH_SEAM_POSITION_MAX +
             MAX_SCREEN_COORD / 2) / MAX_SCREEN_COORD);
        CHECK(state.cursor_crossing.phase == CURSOR_CROSSING_WAITING &&
                  placed_screen == expected_screen && placed_position == expected_position,
              "fast diagonal did not request the adjacent Mac edge position");
        const int16_t entry_x = chain_seam == RIGHT ? MIN_SCREEN_COORD : MAX_SCREEN_COORD;
        CHECK(apply_helper_cursor_position(&state, 1, expected_screen, entry_x,
                                           preserved_along_edge, placement_query_id),
              "fast-diagonal Mac placement confirmation was refused");
        mouse_crossing_task(&state, 1);
        CHECK(state.config.output[1].screen_index == expected_screen &&
                  state.pointer_y == preserved_along_edge && virtual_switches == 0,
              "fast diagonal used legacy reports or lost its destination-edge coordinate");
        placement_query_available = false;
    }
}

static void test_output_arrival_ignores_reverse_jitter_until_motion_turns_inward(void) {
    device_t state = four_screen_corner_state(RIGHT, BOTTOM);
    state.active_output = 0;
    state.config.output[0].screen_index = 1;
    state.config.output[1].screen_index = 2;
    state.config.output[1].speed_y = 32;
    state.pointer_x = 6323;
    state.pointer_y = MAX_SCREEN_COORD;
    global_state = state;
    output_switches = 0;

    do_screen_switch(&state, BOTTOM);
    CHECK(state.active_output == 1 && state.config.output[1].screen_index == 2,
          "arrival-jitter setup did not enter Windows screen 2");

    mouse_values_t jitter = {.move_y = -3};
    const enum screen_pos_e bounced = update_mouse_position(&state, &jitter);
    if (bounced != NONE)
        do_screen_switch(&state, bounced);
    CHECK(bounced == NONE && state.active_output == 1,
          "tiny reverse jitter immediately bounced back across the output seam");

    jitter.move_y = -3;
    CHECK(update_mouse_position(&state, &jitter) == TOP,
          "arrival guard permanently blocked a slow intentional reversal");

    mouse_values_t inward = {.move_y = 3};
    CHECK(update_mouse_position(&state, &inward) == NONE,
          "inward arrival motion unexpectedly crossed a seam");
    state.pointer_y = MIN_SCREEN_COORD;
    mouse_values_t deliberate_return = {.move_y = -16};
    CHECK(update_mouse_position(&state, &deliberate_return) == TOP,
          "arrival guard did not re-arm the seam after inward motion");
}

static void test_output_arrival_survives_perpendicular_monitor_crossing_jitter(void) {
    device_t state = four_screen_corner_state(RIGHT, BOTTOM);
    state.active_output = 0;
    state.config.output[0].os = MACOS;
    state.config.output[1].os = WINDOWS;
    state.config.output[0].screen_index = 2;
    state.config.output[1].screen_index = 1;
    state.config.output[1].chain_direction = DH_DIRECTION_RIGHT;
    state.config.output[1].speed_x = 16;
    state.config.output[1].speed_y = 32;
    state.config.enable_acceleration = true;
    state.pointer_x = 16769;
    state.pointer_y = MAX_SCREEN_COORD;
    global_state = state;
    output_switches = 0;

    do_screen_switch(&state, BOTTOM);
    CHECK(state.active_output == 1 && state.config.output[1].screen_index == 1,
          "perpendicular-jitter setup did not enter Windows screen 1");

    state.pointer_x = 32612;
    state.pointer_y = 24;
    mouse_values_t chain_motion = {.move_x = 46, .move_y = 2};
    const enum screen_pos_e chain_seam = update_mouse_position(&state, &chain_motion);
    CHECK(chain_seam == RIGHT,
          "trace replay did not reach the adjacent Windows monitor");
    do_screen_switch(&state, chain_seam);
    CHECK(state.active_output == 1 && state.config.output[1].screen_index == 2,
          "trace replay did not enter Windows screen 2");

    state.pointer_y = 20;
    mouse_values_t reverse_jitter = {.move_y = -1};
    const enum screen_pos_e bounced = update_mouse_position(&state, &reverse_jitter);
    if (bounced != NONE)
        do_screen_switch(&state, bounced);
    CHECK(bounced == NONE && state.active_output == 1,
          "perpendicular monitor crossing jitter cleared the output-arrival guard");
}

static void test_relative_source_query_retries_transient_interboard_pressure(void) {
    device_t state = stacked_computers_state();
    state.config.output[0].os = WINDOWS;
    state.config.output[0].screen_index = 2;
    state.relative_mouse = true;
    state.pointer_y = MIN_SCREEN_COORD;
    source_query_available = true;
    source_query_retries = 1;
    source_queries = 0;
    output_switches = 0;

    do_screen_switch(&state, TOP);
    CHECK(output_switches == 0 && source_queries == 1 &&
              state.cursor_crossing.phase == CURSOR_CROSSING_WAITING,
          "transient inter-board pressure immediately used stale-coordinate fallback");
    mouse_crossing_task(&state, 30000);
    CHECK(output_switches == 0 && source_queries == 2 &&
              state.cursor_crossing.phase == CURSOR_CROSSING_WAITING,
          "query sent at the enqueue deadline timed out before its response window");
    CHECK(apply_helper_cursor_position(&state, 0, 2, 12000, MIN_SCREEN_COORD,
                                       state.cursor_crossing.query_id),
          "retried source query response was refused");
    mouse_crossing_task(&state, 30001);
    CHECK(output_switches == 1,
          "retried source query did not complete the mapped output crossing");
    source_query_retries = 0;
    source_query_available = false;
}

static void test_relative_source_query_pressure_has_a_bounded_fallback(void) {
    device_t state = stacked_computers_state();
    state.config.output[0].os = WINDOWS;
    state.config.output[0].screen_index = 2;
    state.relative_mouse = true;
    state.pointer_y = MIN_SCREEN_COORD;
    source_query_available = true;
    source_query_retries = 100;
    output_switches = 0;

    do_screen_switch(&state, TOP);
    mouse_crossing_task(&state, 30000);
    mouse_crossing_task(&state, 30001);
    CHECK(output_switches == 1 && state.cursor_crossing.phase == CURSOR_CROSSING_IDLE,
          "persistent inter-board pressure trapped the cursor past the enqueue deadline");
    source_query_retries = 0;
    source_query_available = false;
}

int main(void) {
    test_update_and_switch_at_the_public_mouse_seam();
    test_virtual_desktops_remain_local();
    test_confirmed_macos_chain_forward_uses_only_helper_placement();
    test_confirmed_macos_chain_back_uses_only_helper_placement();
    test_macos_chain_fallbacks_once();
    test_macos_chain_holds_only_position_while_pending();
    test_macos_chain_requires_the_requested_placement_coordinate();
    test_fast_diagonal_macos_chain_uses_correlated_placement();
    test_output_arrival_ignores_reverse_jitter_until_motion_turns_inward();
    test_output_arrival_survives_perpendicular_monitor_crossing_jitter();
    test_relative_source_query_retries_transient_interboard_pressure();
    test_relative_source_query_pressure_has_a_bounded_fallback();
    test_perpendicular_seam_crosses_from_any_monitor();
    test_diagonal_push_uses_the_larger_overshoot();
    test_non_transition_and_guarded_edges_clamp_normally();
    test_diagonal_corner_continues_without_a_synthetic_adjacent_seam();
    test_vertical_seam_preserves_crossing_guards();
    test_jump_threshold_uses_the_vertical_seam_axis();
    test_configured_seam_maps_position_and_blocks_gaps();
    test_mapped_crossing_places_cursor_on_target_monitor();
    test_relative_source_crossing_waits_for_true_position();
    test_fast_diagonal_cannot_change_screens_while_source_query_is_pending();
    test_relative_source_crossing_falls_back_without_helper();
    test_relative_source_crossing_falls_back_on_unavailable_reply();
    test_relative_source_crossing_falls_back_on_timeout();
    test_stranded_resuming_crossing_releases_mouse_input();
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
