/*
 * Host coverage for the mouse layout seam (#24).
 *
 * The firmware adapter owns I/O; this suite drives the pure transition
 * decision with the same per-output state that update_mouse_position() and
 * do_screen_switch() use.
 */
#include <stdio.h>

#include "dh_mouse_layout.h"

static int failures;

#define CHECK(condition, name, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s: %s\n", name, message); \
        failures++; \
    } \
} while (0)

static void test_side_by_side_layout_preserves_current_switching(void) {
    const dh_mouse_layout_t output_a = {
        .chain_direction = DH_DIRECTION_RIGHT,
        .border_direction = DH_DIRECTION_LEFT,
    };

    CHECK(dh_mouse_transition_for(&output_a, 1, 2, DH_DIRECTION_LEFT) ==
              DH_MOUSE_TRANSITION_OUTPUT,
          "side_by_side", "output A did not cross left from its border-adjacent monitor");
    CHECK(dh_mouse_transition_for(&output_a, 1, 2, DH_DIRECTION_RIGHT) ==
              DH_MOUSE_TRANSITION_CHAIN_FORWARD,
          "side_by_side", "output A did not move right to its second monitor");
    CHECK(dh_mouse_transition_for(&output_a, 2, 2, DH_DIRECTION_LEFT) ==
              DH_MOUSE_TRANSITION_CHAIN_BACK,
          "side_by_side", "output A crossed computers from a non-border monitor");
    CHECK(dh_mouse_transition_for(&output_a, 2, 2, DH_DIRECTION_RIGHT) ==
              DH_MOUSE_TRANSITION_NONE,
          "side_by_side", "output A moved past the end of its monitor chain");

    CHECK(dh_mouse_jump_threshold_for(&output_a, 1, 2, DH_DIRECTION_LEFT, 700) == 700,
          "threshold", "the inter-computer crossing lost its configured threshold");
    CHECK(dh_mouse_jump_threshold_for(&output_a, 1, 2, DH_DIRECTION_RIGHT, 700) == 0,
          "threshold", "a local virtual-desktop switch gained a jump threshold");
    CHECK(dh_mouse_next_screen_index(DH_MOUSE_TRANSITION_CHAIN_FORWARD, 1) == 2,
          "virtual_desktop", "the forward chain transition chose the wrong screen");
    CHECK(dh_mouse_next_screen_index(DH_MOUSE_TRANSITION_CHAIN_BACK, 2) == 1,
          "virtual_desktop", "the backward chain transition chose the wrong screen");

    CHECK(dh_mouse_park_coordinate(0, 1234, 0, 32767) == 0,
          "parking", "top parking moved away from the top coordinate");
    CHECK(dh_mouse_park_coordinate(1, 1234, 0, 32767) == 32767,
          "parking", "bottom parking moved away from the bottom coordinate");
    CHECK(dh_mouse_park_coordinate(3, 1234, 0, 32767) == 1234,
          "parking", "previous-position parking stopped preserving the coordinate");
}

static void test_chain_direction_is_independent_of_the_border(void) {
    const dh_mouse_layout_t perpendicular = {
        .chain_direction = DH_DIRECTION_RIGHT,
        .border_direction = DH_DIRECTION_TOP,
    };

    CHECK(dh_mouse_transition_for(&perpendicular, 2, 2, DH_DIRECTION_LEFT) ==
              DH_MOUSE_TRANSITION_CHAIN_BACK,
          "independent_axes", "chain-back movement was derived from the border direction");
    CHECK(dh_mouse_transition_for(&perpendicular, 2, 2, DH_DIRECTION_TOP) ==
              DH_MOUSE_TRANSITION_NONE,
          "independent_axes", "perpendicular crossing behavior arrived before issue #28");
}

int main(void) {
    test_side_by_side_layout_preserves_current_switching();
    test_chain_direction_is_independent_of_the_border();

    if (failures != 0)
        return 1;

    printf("mouse_layout_test: all checks passed\n");
    return 0;
}
