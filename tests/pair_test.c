/*
 * Pairing tests for the shared core (#46): the window, rotation, and the
 * authentication that everything else on the channel is gated on.
 *
 * These assert a security control, so they are written against the attack
 * rather than against the API: can a secret be obtained outside a window,
 * does rotating actually evict, and is a stolen pairing recoverable.
 *
 * Style follows frame_test.c: an assertion macro, a main, a printed failure
 * line, a non-zero exit — no framework.
 */

#include <stdio.h>
#include <string.h>

#include "dh_pair.h"

static int failures = 0;

#define CHECK(cond, name, what)                                                 \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++failures;                                                         \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, (name), (what)); \
        }                                                                       \
    } while (0)

static const uint8_t secret_a[DH_PAIR_SECRET_LEN] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
};
static const uint8_t secret_b[DH_PAIR_SECRET_LEN] = {
    0xf0, 0xe1, 0xd2, 0xc3, 0xb4, 0xa5, 0x96, 0x87,
    0x78, 0x69, 0x5a, 0x4b, 0x3c, 0x2d, 0x1e, 0x0f,
};

static void test_a_fresh_device_authenticates_nobody(void) {
    dh_pair p;
    dh_pair_init(&p, NULL);

    CHECK(!dh_pair_authenticate(&p, secret_a, sizeof secret_a), "fresh",
          "a device with no secret authenticated a helper");

    /* And it hands nothing out until a chord has been pressed. */
    uint8_t granted[DH_PAIR_SECRET_LEN];
    CHECK(!dh_pair_grant(&p, 1000, granted), "fresh", "granted a secret with no window open");
}

static void test_a_stored_secret_survives_a_restart(void) {
    /* What the firmware does at boot: read the secret back from flash. */
    dh_pair p;
    dh_pair_init(&p, secret_a);

    CHECK(dh_pair_authenticate(&p, secret_a, sizeof secret_a), "restart",
          "a stored secret did not authenticate its helper");
    CHECK(!dh_pair_authenticate(&p, secret_b, sizeof secret_b), "restart",
          "the wrong secret authenticated");
}

static void test_a_wrong_token_is_refused_however_close(void) {
    dh_pair p;
    dh_pair_init(&p, secret_a);

    /* One byte out, at each end and in the middle. A near miss is a miss. */
    for (size_t i = 0; i < DH_PAIR_SECRET_LEN; i++) {
        uint8_t almost[DH_PAIR_SECRET_LEN];
        memcpy(almost, secret_a, sizeof almost);
        almost[i] ^= 0x01;
        CHECK(!dh_pair_authenticate(&p, almost, sizeof almost), "near-miss",
              "a secret differing by one bit authenticated");
    }

    /* Length is part of it: a prefix of the secret is not the secret. */
    CHECK(!dh_pair_authenticate(&p, secret_a, DH_PAIR_SECRET_LEN - 1), "near-miss",
          "a truncated secret authenticated");
    CHECK(!dh_pair_authenticate(&p, secret_a, DH_PAIR_SECRET_LEN + 1), "near-miss",
          "an over-long secret authenticated");
    CHECK(!dh_pair_authenticate(&p, NULL, DH_PAIR_SECRET_LEN), "near-miss",
          "a null token authenticated");
}

static void test_the_window_opens_for_about_a_minute(void) {
    dh_pair p;
    dh_pair_init(&p, NULL);

    const uint32_t pressed = 500000;
    dh_pair_open_window(&p, secret_a, pressed);

    CHECK(dh_pair_window_open(&p, pressed), "window", "the window did not open");
    CHECK(dh_pair_window_open(&p, pressed + DH_PAIR_WINDOW_MS - 1), "window",
          "the window closed early");
    CHECK(!dh_pair_window_open(&p, pressed + DH_PAIR_WINDOW_MS), "window",
          "the window did not close on time");

    /* A helper connecting during the window is provisioned; one connecting
       after it is not. Nothing about that depends on the user. */
    uint8_t granted[DH_PAIR_SECRET_LEN];
    CHECK(dh_pair_grant(&p, pressed + 1000, granted), "window", "no secret inside the window");
    CHECK(memcmp(granted, secret_a, sizeof granted) == 0, "window", "granted the wrong secret");

    dh_pair_tick(&p, pressed + DH_PAIR_WINDOW_MS);
    memset(granted, 0, sizeof granted);
    CHECK(!dh_pair_grant(&p, pressed + DH_PAIR_WINDOW_MS, granted), "window",
          "a secret was handed out after the window closed");

    /* Refused means nothing written, not a zeroed secret handed over. */
    uint8_t zeroes[DH_PAIR_SECRET_LEN] = {0};
    CHECK(memcmp(granted, zeroes, sizeof granted) == 0, "window",
          "a refused grant wrote to the caller's buffer");

    /* The pairing it made still works: closing the window is not a wipe. */
    CHECK(dh_pair_authenticate(&p, secret_a, sizeof secret_a), "window",
          "closing the window unpaired the helper it provisioned");
}

static void test_the_window_survives_the_clock_wrapping(void) {
    dh_pair p;
    dh_pair_init(&p, NULL);

    const uint32_t before_wrap = UINT32_MAX - (DH_PAIR_WINDOW_MS / 2);
    dh_pair_open_window(&p, secret_a, before_wrap);

    const uint32_t after_wrap = before_wrap + (DH_PAIR_WINDOW_MS / 2) + 1000; /* wraps */
    CHECK(dh_pair_window_open(&p, after_wrap), "wrap", "the window closed across the wrap");
    CHECK(!dh_pair_window_open(&p, before_wrap + DH_PAIR_WINDOW_MS), "wrap",
          "the window never closed across the wrap");
}

/*
 * The decision this ticket carries: every window rotates the secret, so a
 * pairing that leaked is revocable by the same one-keystroke gesture that
 * created it. Walked through as the attack it exists for.
 */
static void test_rotation_makes_a_stolen_pairing_recoverable(void) {
    dh_pair p;
    dh_pair_init(&p, NULL);

    /* Monday: the user pairs their helper. */
    dh_pair_open_window(&p, secret_a, 1000);
    uint8_t helper_secret[DH_PAIR_SECRET_LEN];
    CHECK(dh_pair_grant(&p, 1000, helper_secret), "rotation", "the first pairing failed");
    CHECK(dh_pair_authenticate(&p, helper_secret, sizeof helper_secret), "rotation",
          "the paired helper was not authenticated");

    /* Tuesday: a process that won the exclusivity race is connected when the
       user presses the chord, and is provisioned. This is the attack #34
       accepts as a residual risk, not one this control prevents. */
    const uint32_t tuesday = 1000 + 86400000u;
    dh_pair_open_window(&p, secret_b, tuesday);
    uint8_t stolen[DH_PAIR_SECRET_LEN];
    CHECK(dh_pair_grant(&p, tuesday, stolen), "rotation", "the window did not provision");
    CHECK(dh_pair_authenticate(&p, stolen, sizeof stolen), "rotation",
          "the stolen secret does not work — the scenario is wrong");

    /* That press already evicted the user's own helper: rotation is what
       makes the eviction, and the recovery, the same gesture. */
    CHECK(!dh_pair_authenticate(&p, helper_secret, sizeof helper_secret), "rotation",
          "the previous pairing survived a rotation");

    /* Wednesday: the user works out what happened, stops the process, and
       presses the chord once with only their own helper connected. */
    const uint32_t wednesday = tuesday + 86400000u;
    const uint8_t secret_c[DH_PAIR_SECRET_LEN] = {
        0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00, 0x11,
        0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99,
    };
    dh_pair_open_window(&p, secret_c, wednesday);
    uint8_t repaired[DH_PAIR_SECRET_LEN];
    CHECK(dh_pair_grant(&p, wednesday, repaired), "rotation", "re-pairing failed");

    /* The theft is revoked, and it cost one keystroke rather than a
       configuration wipe. */
    CHECK(!dh_pair_authenticate(&p, stolen, sizeof stolen), "rotation",
          "a stolen pairing survived the recovery press — rotation is not working");
    CHECK(dh_pair_authenticate(&p, repaired, sizeof repaired), "rotation",
          "the user's own helper was not re-paired");
}

/* A window opened while one is already open re-rotates: holding the chord, or
 * bouncing in and out of config mode, must not leave two live secrets. */
static void test_reopening_a_window_rotates_again(void) {
    dh_pair p;
    dh_pair_init(&p, NULL);

    dh_pair_open_window(&p, secret_a, 1000);
    dh_pair_open_window(&p, secret_b, 2000);

    CHECK(!dh_pair_authenticate(&p, secret_a, sizeof secret_a), "reopen",
          "the first secret of two overlapping windows still authenticates");
    CHECK(dh_pair_authenticate(&p, secret_b, sizeof secret_b), "reopen",
          "the second secret does not authenticate");
    CHECK(dh_pair_window_open(&p, 2000 + DH_PAIR_WINDOW_MS - 1), "reopen",
          "reopening did not extend the window");
}

int main(void) {
    test_a_fresh_device_authenticates_nobody();
    test_a_stored_secret_survives_a_restart();
    test_a_wrong_token_is_refused_however_close();
    test_the_window_opens_for_about_a_minute();
    test_the_window_survives_the_clock_wrapping();
    test_rotation_makes_a_stolen_pairing_recoverable();
    test_reopening_a_window_rotates_again();

    if (failures) {
        printf("%d pairing check(s) failed\n", failures);
        return 1;
    }
    printf("pairing tests passed\n");
    return 0;
}
