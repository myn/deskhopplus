/*
 * The board's identity and its one registration (#111, ADR-0008).
 *
 * What this file is really testing is the property that replaced the bearer
 * token: **only public halves cross**. A registration is what one ECDH
 * produced from the board's private key and the helper's public one, and the
 * helper computes the same value from the other two halves — so the check that
 * matters is that both ends reach the same 32 bytes without either private key
 * ever appearing on the wire. That number is not this repository's to invent,
 * so it is read out of test-vectors/primitives.txt, where an independent
 * implementation put it (#109, #110).
 *
 * The rest is the window: single-shot, a minute long, wrap-safe, and — unlike
 * v1's — not something a chord press spends when nobody pairs against it.
 *
 * Style follows frame_test.c: an assertion macro, a main, a printed failure
 * line, a non-zero exit — no framework.
 */

#include <ctype.h>
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

/* ------------------------------------------------------------------ loading */

#define MAX_FIELDS 8
#define MAX_FIELD_BYTES 64

/* The published pairing material: helper private, board private, the two
   nonces, then the shared secret and the three session keys. Only the first
   two and the fifth are used here; the keys are auth_test's business. */
static uint8_t field[MAX_FIELDS][MAX_FIELD_BYTES];
static size_t field_len[MAX_FIELDS];

static int hex_nibble(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* `session_material | <hex> | <hex> ...` out of primitives.txt. One line, so
   this is deliberately smaller than auth_test's general loader. */
static bool load_session_material(const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) {
        printf("FAIL cannot open %s\n", path);
        return false;
    }

    char line[8192];
    bool found = false;
    while (!found && fgets(line, sizeof line, file)) {
        if (strncmp(line, "session_material", 16) != 0) continue;
        char *bar = strchr(line, '|');
        if (!bar) break;

        size_t fields = 0;
        for (char *q = bar; *q && fields < MAX_FIELDS;) {
            const size_t idx = fields++;
            field_len[idx] = 0;
            int hi = -1;
            for (q++; *q && *q != '|'; q++) {
                if (isspace((unsigned char)*q)) continue;
                const int nib = hex_nibble((unsigned char)*q);
                if (nib < 0 || field_len[idx] >= MAX_FIELD_BYTES) goto done;
                if (hi < 0) {
                    hi = nib;
                } else {
                    field[idx][field_len[idx]++] = (uint8_t)((hi << 4) | nib);
                    hi = -1;
                }
            }
        }
        found = fields >= 5;
    }
done:
    fclose(file);
    if (!found) printf("FAIL session_material missing or short in %s\n", path);
    return found;
}

#define HELPER_PRIVATE field[0]
#define BOARD_PRIVATE field[1]
#define PUBLISHED_SHARED field[4]

static uint8_t helper_public[DH_P256_PUBLIC_SIZE];

/* A board holding the published identity, and nothing else. */
static void a_board(dh_pair *p) {
    dh_pair_init(p);
    CHECK(dh_pair_set_identity(p, BOARD_PRIVATE), "setup", "the published board key was refused");
}

/* -------------------------------------------------------------------- tests */

static void test_a_fresh_board_has_neither_identity_nor_registration(void) {
    dh_pair p;
    dh_pair_init(&p);

    CHECK(!dh_pair_has_identity(&p), "fresh", "a board had an identity before flash was read");
    CHECK(dh_pair_public_key(&p) == NULL, "fresh",
          "a board with no identity handed out a public key");
    CHECK(!dh_pair_registered(&p), "fresh", "a board was registered before anything paired");
    CHECK(dh_pair_shared_secret(&p) == NULL, "fresh",
          "a board with no registration handed out a shared secret");

    /* And it cannot pair, even inside a window: there is no private half to
       agree with. This is only reachable before first boot's generation. */
    dh_pair_open_window(&p, 1000);
    CHECK(dh_pair_register(&p, 1000, helper_public) == DH_PAIR_ERR_NO_IDENTITY, "fresh",
          "a board with no identity completed a pairing");
}

/*
 * The property the whole design rests on: the board reaches the same 32 bytes
 * the helper does, from the two public halves and its own private one, and the
 * value matches what an independent implementation published.
 */
static void test_pairing_reaches_the_published_shared_secret(void) {
    dh_pair p;
    a_board(&p);
    dh_pair_open_window(&p, 5000);

    CHECK(dh_pair_register(&p, 5000, helper_public) == DH_PAIR_OK, "ecdh",
          "a pairing inside an open window was refused");
    CHECK(dh_pair_registered(&p), "ecdh", "a completed pairing left the board unregistered");

    const uint8_t *shared = dh_pair_shared_secret(&p);
    CHECK(shared != NULL, "ecdh", "a registered board has no shared secret");
    CHECK(shared != NULL && field_len[4] == DH_P256_SHARED_SIZE &&
              memcmp(shared, PUBLISHED_SHARED, DH_P256_SHARED_SIZE) == 0,
          "ecdh", "the board's shared secret is not the published one");

    /* The key id names the helper's key without carrying it. */
    uint8_t expected[DH_KEY_ID_SIZE];
    dh_p256_key_id(helper_public, expected);
    CHECK(dh_pair_is_registered_key(&p, expected), "ecdh",
          "the registered key id is not the helper key that paired");

    uint8_t someone_else[DH_KEY_ID_SIZE];
    memcpy(someone_else, expected, sizeof someone_else);
    someone_else[0] ^= 1u;
    CHECK(!dh_pair_is_registered_key(&p, someone_else), "ecdh",
          "a different key id was taken for the registered one");
}

/*
 * The window is single-shot: the first registration closes it. A listener can
 * no longer be provisioned silently *alongside* the helper — if it wins the
 * race it is registered and the helper is not, and the helper is told which of
 * those happened.
 */
static void test_the_first_registration_closes_the_window(void) {
    dh_pair p;
    a_board(&p);

    CHECK(dh_pair_register(&p, 1000, helper_public) == DH_PAIR_ERR_NO_WINDOW, "single shot",
          "a pairing outside any window was granted");

    dh_pair_open_window(&p, 2000);
    CHECK(dh_pair_window_open(&p, 2000), "single shot", "a fresh window was not open");
    CHECK(dh_pair_register(&p, 2000, helper_public) == DH_PAIR_OK, "single shot",
          "the first registration was refused");
    CHECK(!dh_pair_window_open(&p, 2000), "single shot",
          "the window stayed open after a registration");

    /* The second asker is told *why*, and it is not the same answer as
       arriving with no window at all: the user pressed the chord, and
       something took the window they opened. */
    CHECK(dh_pair_register(&p, 2001, helper_public) == DH_PAIR_ERR_ALREADY_REGISTERED,
          "single shot", "a second registration was not told the window had been claimed");
}

/* Split out of the test above, because the clock has to advance through
   dh_pair_tick the way the firmware advances it. */
static void test_a_claimed_window_stops_being_claimed_after_its_minute(void) {
    dh_pair p;
    a_board(&p);
    dh_pair_open_window(&p, 2000);
    CHECK(dh_pair_register(&p, 2000, helper_public) == DH_PAIR_OK, "claimed", "no registration");

    dh_pair_tick(&p, 2000 + DH_PAIR_WINDOW_MS - 1u);
    CHECK(dh_pair_register(&p, 2000 + DH_PAIR_WINDOW_MS - 1u, helper_public) ==
              DH_PAIR_ERR_ALREADY_REGISTERED,
          "claimed", "the claim expired before the minute did");

    dh_pair_tick(&p, 2000 + DH_PAIR_WINDOW_MS);
    CHECK(dh_pair_register(&p, 2000 + DH_PAIR_WINDOW_MS, helper_public) == DH_PAIR_ERR_NO_WINDOW,
          "claimed", "the claim outlived the window it belonged to");
}

/*
 * v1 rotated the device secret on every chord press, because a leaked bearer
 * token stayed valid until the configuration was wiped — and ADR-0008 recorded
 * that where the channel is not exclusive, rotating re-issued the pairing to
 * whatever was listening. Nothing secret crosses now, so a press nobody pairs
 * against costs the user nothing.
 */
static void test_a_window_nobody_uses_leaves_the_registration_alone(void) {
    dh_pair p;
    a_board(&p);
    dh_pair_open_window(&p, 1000);
    CHECK(dh_pair_register(&p, 1000, helper_public) == DH_PAIR_OK, "unused window", "no pairing");

    uint8_t was[DH_P256_SHARED_SIZE];
    memcpy(was, dh_pair_shared_secret(&p), sizeof was);

    /* A second chord press, and the minute runs out with nobody asking. */
    dh_pair_open_window(&p, 100000);
    CHECK(dh_pair_registered(&p), "unused window", "opening a window unpaired the board");
    dh_pair_tick(&p, 100000 + DH_PAIR_WINDOW_MS);
    CHECK(!dh_pair_window_open(&p, 100000 + DH_PAIR_WINDOW_MS), "unused window",
          "the window never closed");

    CHECK(dh_pair_registered(&p), "unused window", "an unused window unpaired the board");
    CHECK(memcmp(dh_pair_shared_secret(&p), was, sizeof was) == 0, "unused window",
          "an unused window changed the shared secret");
}

/* Re-pairing replaces the registration — one board serves one computer, so
   there is exactly one, and that is what makes a stolen pairing recoverable. */
static void test_re_pairing_replaces_the_registration(void) {
    dh_pair p;
    a_board(&p);
    dh_pair_open_window(&p, 1000);
    CHECK(dh_pair_register(&p, 1000, helper_public) == DH_PAIR_OK, "re-pair", "no first pairing");

    uint8_t other_private[DH_P256_PRIVATE_SIZE];
    uint8_t other_public[DH_P256_PUBLIC_SIZE];
    memcpy(other_private, HELPER_PRIVATE, sizeof other_private);
    other_private[31] ^= 0x0Fu;
    CHECK(dh_p256_public_from_private(other_private, other_public), "re-pair",
          "the second key would not derive");

    uint8_t first[DH_P256_SHARED_SIZE];
    memcpy(first, dh_pair_shared_secret(&p), sizeof first);

    dh_pair_open_window(&p, 50000);
    CHECK(dh_pair_register(&p, 50000, other_public) == DH_PAIR_OK, "re-pair",
          "a second pairing in a fresh window was refused");
    CHECK(memcmp(dh_pair_shared_secret(&p), first, sizeof first) != 0, "re-pair",
          "re-pairing left the previous shared secret in place");

    uint8_t previous_id[DH_KEY_ID_SIZE];
    dh_p256_key_id(helper_public, previous_id);
    CHECK(!dh_pair_is_registered_key(&p, previous_id), "re-pair",
          "the board still recognises the helper it replaced");
}

/*
 * A pairing request is hostile input: anything attached to the channel can
 * send one. A key that is not a point on the curve is refused, and — this is
 * the part worth pinning — it does **not** burn the window the user opened.
 */
static void test_a_bad_key_is_refused_without_spending_the_window(void) {
    dh_pair p;
    a_board(&p);
    dh_pair_open_window(&p, 1000);

    uint8_t not_a_point[DH_P256_PUBLIC_SIZE];
    memset(not_a_point, 0xAA, sizeof not_a_point);
    CHECK(dh_pair_register(&p, 1000, not_a_point) == DH_PAIR_ERR_BAD_KEY, "bad key",
          "a point off the curve was registered");
    CHECK(!dh_pair_registered(&p), "bad key", "a refused key still left a registration");
    CHECK(dh_pair_window_open(&p, 1000), "bad key",
          "a garbage request closed the window the user opened");

    /* The real helper, arriving second, still gets its pairing. */
    CHECK(dh_pair_register(&p, 1001, helper_public) == DH_PAIR_OK, "bad key",
          "the window was unusable after a garbage request");
}

/*
 * A wipe unpairs and nothing more. If it took the identity too, every wipe
 * would make every helper report "this board changed" — a false alarm on a
 * routine action (#75, ADR-0008).
 */
static void test_a_wipe_unpairs_and_leaves_the_identity(void) {
    dh_pair p;
    a_board(&p);
    dh_pair_open_window(&p, 1000);
    CHECK(dh_pair_register(&p, 1000, helper_public) == DH_PAIR_OK, "wipe", "no pairing");

    uint8_t was_public[DH_P256_PUBLIC_SIZE];
    memcpy(was_public, dh_pair_public_key(&p), sizeof was_public);

    dh_pair_clear_registration(&p);

    CHECK(!dh_pair_registered(&p), "wipe", "a wipe left the board paired");
    CHECK(dh_pair_shared_secret(&p) == NULL, "wipe", "a wipe left the shared secret readable");
    CHECK(dh_pair_has_identity(&p), "wipe", "a wipe took the board's identity");
    CHECK(memcmp(dh_pair_public_key(&p), was_public, sizeof was_public) == 0, "wipe",
          "a wipe changed who this board is");

    /* And a chord press after the wipe pairs it again, which is the whole
       recovery: one keystroke. */
    dh_pair_open_window(&p, 2000);
    CHECK(dh_pair_register(&p, 2000, helper_public) == DH_PAIR_OK, "wipe",
          "a wiped board could not be re-paired");
}

/* A wrapping millisecond counter is arithmetic, not a window that never
   closes — nor one that closes the moment it opens. */
static void test_the_window_survives_the_clock_wrapping(void) {
    dh_pair p;
    a_board(&p);

    const uint32_t before_wrap = UINT32_MAX - (DH_PAIR_WINDOW_MS / 2u);
    dh_pair_open_window(&p, before_wrap);

    const uint32_t after_wrap = before_wrap + (DH_PAIR_WINDOW_MS / 2u) + 1000u; /* wraps */
    CHECK(dh_pair_window_open(&p, after_wrap), "wrap", "the window closed across the wrap");

    dh_pair_tick(&p, before_wrap + DH_PAIR_WINDOW_MS);
    CHECK(!dh_pair_window_open(&p, before_wrap + DH_PAIR_WINDOW_MS), "wrap",
          "the window never closed across the wrap");
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : DH_PRIMITIVE_VECTORS;
    if (!load_session_material(path)) return 1;

    if (!dh_p256_public_from_private(HELPER_PRIVATE, helper_public)) {
        printf("FAIL the published helper key would not derive\n");
        return 1;
    }

    test_a_fresh_board_has_neither_identity_nor_registration();
    test_pairing_reaches_the_published_shared_secret();
    test_the_first_registration_closes_the_window();
    test_a_claimed_window_stops_being_claimed_after_its_minute();
    test_a_window_nobody_uses_leaves_the_registration_alone();
    test_re_pairing_replaces_the_registration();
    test_a_bad_key_is_refused_without_spending_the_window();
    test_a_wipe_unpairs_and_leaves_the_identity();
    test_the_window_survives_the_clock_wrapping();

    if (failures) {
        printf("%d pairing check(s) failed\n", failures);
        return 1;
    }
    printf("pairing tests passed\n");
    return 0;
}
