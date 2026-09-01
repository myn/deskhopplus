/*
 * The cursor of a config Read All (#156).
 *
 * GET_ALL_VALS_MSG used to queue one HID response for every mapped field in a
 * single pass. Two requests arriving before the first drained asked the queue
 * for more than it holds, and queue_try_add's refusal was ignored, so the tail
 * of the second response was dropped without a word.
 *
 * The walk hands out one field per drain pass instead, so the queue never has
 * to hold the whole map. What has to be right is small and entirely arithmetic:
 * every field is handed out exactly once, a response the queue refused is
 * offered again rather than skipped, and a restart mid-walk still covers the
 * whole map. All three sat inside a handler that needs a device to reach.
 *
 * Style follows config_test.c: an assertion macro, a main, a printed failure
 * line, a non-zero exit — no framework.
 */

#include <stdio.h>
#include <string.h>

#include "config_read_all.h"

static int failures = 0;

#define CHECK(cond, name, what)                                                 \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++failures;                                                         \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, (name), (what)); \
        }                                                                       \
    } while (0)

/* Deliberately not get_field_map_length(): the walk's arithmetic is the same
   whatever the map holds, and tying the test to the live map would let a map
   change quietly move what is being checked. The cast keeps MSVC's /W4 quiet
   about narrowing to the uint16_t the walk counts in. */
#define FIELDS ((uint16_t)135)

/* Drain a walk to exhaustion, recording how often each index was handed out.
   Returns the number of passes it took. */
static unsigned drain(config_read_all_t *walk, unsigned *seen, unsigned cap) {
    unsigned passes = 0;
    uint16_t index;

    while (config_read_all_peek(walk, &index)) {
        CHECK(index < cap, "drain", "index outside the field map");
        if (index < cap)
            seen[index]++;
        config_read_all_sent(walk, index);
        if (++passes > cap * 4u)
            break; /* a walk that never ends; the caller's count says so */
    }
    return passes;
}

static void every_field_once(void) {
    config_read_all_t walk;
    unsigned seen[FIELDS] = {0};

    config_read_all_start(&walk, FIELDS);
    unsigned passes = drain(&walk, seen, FIELDS);

    CHECK(passes == FIELDS, "every field once", "walk did not take one pass per field");

    for (unsigned i = 0; i < FIELDS; i++)
        CHECK(seen[i] == 1, "every field once", "a field was skipped or repeated");
}

/* The queue is shared, so a response can be refused even when the walk
   checked for room. A refused field is the same field next pass — the silent
   skip is the defect this whole walk exists to remove. */
static void a_refused_field_is_offered_again(void) {
    config_read_all_t walk;
    uint16_t first, again;

    config_read_all_start(&walk, FIELDS);

    CHECK(config_read_all_peek(&walk, &first), "refused field", "walk went idle at once");
    CHECK(config_read_all_peek(&walk, &again), "refused field", "walk went idle without a send");
    CHECK(first == again, "refused field", "walk moved on without the response being queued");

    config_read_all_sent(&walk, first);
    CHECK(config_read_all_peek(&walk, &again), "refused field", "walk ended after one field");
    CHECK(again == first + 1, "refused field", "walk did not advance after a queued response");
}

/* The reported failure: a second Read All arrives before the first has
   drained. It restarts the walk rather than stacking a second map's worth of
   responses on the queue, and the page still ends up with every field. */
static void a_restart_still_covers_the_map(void) {
    config_read_all_t walk;
    unsigned seen[FIELDS] = {0};
    uint16_t index;

    config_read_all_start(&walk, FIELDS);
    for (unsigned i = 0; i < FIELDS / 2u; i++) {
        CHECK(config_read_all_peek(&walk, &index), "restart", "first walk ended early");
        config_read_all_sent(&walk, index);
    }

    config_read_all_start(&walk, FIELDS);
    unsigned passes = drain(&walk, seen, FIELDS);

    CHECK(passes == FIELDS, "restart", "restarted walk did not run the whole map");
    for (unsigned i = 0; i < FIELDS; i++)
        CHECK(seen[i] == 1, "restart", "restarted walk skipped or repeated a field");
}

/* Outside config mode the vendor slot belongs to the helper, so every
   response is refused and a walk left running would offer the same field for
   ever. It is abandoned instead. */
static void a_stopped_walk_is_idle(void) {
    config_read_all_t walk;
    uint16_t index;

    config_read_all_start(&walk, FIELDS);
    config_read_all_stop(&walk);

    CHECK(!config_read_all_peek(&walk, &index), "stop", "stopped walk still offers a field");

    config_read_all_start(&walk, FIELDS);
    CHECK(config_read_all_peek(&walk, &index) && index == 0, "stop",
          "a walk cannot be started again after being stopped");
}

/* A request can arrive on the other core, so a walk can be restarted between
   a step peeking at a field and confirming that field was queued. The
   confirmation then belongs to a walk that no longer exists — taking it would
   move the new walk to index 1 and the first field of the map would go
   unread. */
static void a_stale_confirmation_is_ignored(void) {
    config_read_all_t walk;
    uint16_t peeked, index;

    config_read_all_start(&walk, FIELDS);
    for (unsigned i = 0; i < 50u; i++) {
        CHECK(config_read_all_peek(&walk, &peeked), "stale confirm", "walk ended early");
        config_read_all_sent(&walk, peeked);
    }

    CHECK(config_read_all_peek(&walk, &peeked), "stale confirm", "walk ended early");
    config_read_all_start(&walk, FIELDS);  /* the other core restarts it */
    config_read_all_sent(&walk, peeked);   /* the step confirms the old field */

    CHECK(config_read_all_peek(&walk, &index), "stale confirm", "restarted walk is idle");
    CHECK(index == 0, "stale confirm", "restarted walk skipped the first field");
}

int main(void) {
    every_field_once();
    a_refused_field_is_offered_again();
    a_restart_still_covers_the_map();
    a_stopped_walk_is_idle();
    a_stale_confirmation_is_ignored();

    if (failures) {
        printf("config_read_all_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("config_read_all_test: ok\n");
    return 0;
}
