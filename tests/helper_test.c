/*
 * The helper's side of the v2 session (#79, #80): the hello exchange,
 * negotiation, liveness, acquisition, pairing, and the states a user is shown.
 *
 * The other end of every round trip here is the **real board** — dh_session,
 * driven frame by frame — rather than bytes this file made up. The two ends
 * agreeing is the point of lifting this machine into the core at all, so a
 * test where the helper talks to a mock of the board would be checking the
 * mock.
 *
 * The one place golden bytes are used instead is the hello: it is gated
 * against test-vectors/frames.txt, so this file cannot agree with itself about
 * what a hello looks like.
 *
 * Style follows session_test.c: an assertion macro, a main, a printed failure
 * line, a non-zero exit — no framework.
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "dh_helper.h"

static int failures = 0;

#define CHECK(cond, name, what)                                                 \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++failures;                                                         \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, (name), (what)); \
        }                                                                       \
    } while (0)

/* ------------------------------------------------------------------ loading */

#define MAX_VECTORS 128
#define MAX_FIELDS 8
#define MAX_FIELD_BYTES 256

struct vector {
    char name[48];
    size_t fields;
    uint8_t f[MAX_FIELDS][MAX_FIELD_BYTES];
    size_t len[MAX_FIELDS];
};

static struct vector vectors[MAX_VECTORS];
static size_t vector_count;

static uint8_t helper_private[DH_P256_PRIVATE_SIZE];
static uint8_t helper_public[DH_P256_PUBLIC_SIZE];
static uint8_t board_private[DH_P256_PRIVATE_SIZE];
static uint8_t board_public[DH_P256_PUBLIC_SIZE];
static uint8_t published_helper_nonce[DH_NONCE_SIZE];
static uint8_t published_board_nonce[DH_NONCE_SIZE];
static uint8_t shared_secret[DH_P256_SHARED_SIZE];
static uint8_t k_b2h[DH_SESSION_KEY_SIZE];
static uint8_t helper_key_id[DH_KEY_ID_SIZE];

static int hex_nibble(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* `<name> | <hex> | <hex> ...`, comments on '#'. The same format both vector
   files use; frames.txt is its one-field case. */
static bool load_vectors(const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) {
        ++failures;
        printf("FAIL cannot open %s\n", path);
        return false;
    }

    char line[16384];
    while (fgets(line, sizeof line, file)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '\n' || *p == '#') continue;
        char *bar = strchr(p, '|');
        if (!bar) continue;
        if (vector_count >= MAX_VECTORS) {
            ++failures;
            printf("FAIL vector capacity (%d) exceeded in %s\n", MAX_VECTORS, path);
            break;
        }

        struct vector *v = &vectors[vector_count];
        size_t name_len = 0;
        for (char *q = p; q < bar && name_len + 1 < sizeof v->name; q++)
            if (!isspace((unsigned char)*q)) v->name[name_len++] = *q;
        v->name[name_len] = '\0';

        v->fields = 0;
        bool bad = false;
        for (char *q = bar; *q && v->fields < MAX_FIELDS;) {
            const size_t idx = v->fields++;
            v->len[idx] = 0;
            int hi = -1;
            for (q++; *q && *q != '|'; q++) {
                if (isspace((unsigned char)*q)) continue;
                const int nib = hex_nibble((unsigned char)*q);
                if (nib < 0 || v->len[idx] >= MAX_FIELD_BYTES) { bad = true; break; }
                if (hi < 0) {
                    hi = nib;
                } else {
                    v->f[idx][v->len[idx]++] = (uint8_t)((hi << 4) | nib);
                    hi = -1;
                }
            }
            if (bad) break;
            while (*q && *q != '|') q++;
        }
        if (!bad) vector_count++;
    }

    fclose(file);
    return true;
}

static const struct vector *find(const char *name) {
    for (size_t i = 0; i < vector_count; i++)
        if (strcmp(vectors[i].name, name) == 0) return &vectors[i];
    ++failures;
    printf("FAIL vector %s missing\n", name);
    return NULL;
}

static bool load_session_material(void) {
    const struct vector *m = find("session_material");
    if (m == NULL || m->fields < 8) return false;

    memcpy(helper_private, m->f[0], DH_P256_PRIVATE_SIZE);
    memcpy(board_private, m->f[1], DH_P256_PRIVATE_SIZE);
    memcpy(published_helper_nonce, m->f[2], DH_NONCE_SIZE);
    memcpy(published_board_nonce, m->f[3], DH_NONCE_SIZE);
    memcpy(shared_secret, m->f[4], DH_P256_SHARED_SIZE);
    memcpy(k_b2h, m->f[7], DH_SESSION_KEY_SIZE);

    if (!dh_p256_public_from_private(helper_private, helper_public)) return false;
    if (!dh_p256_public_from_private(board_private, board_public)) return false;
    dh_p256_key_id(helper_public, helper_key_id);
    return true;
}

/* ----------------------------------------------------------------- identity */

/*
 * A software stand-in for the Secure Enclave. The seam it exercises is the one
 * the enclave actually offers — one ECDH — so a test passing here says
 * something about the macOS binding rather than only about this file.
 */
static bool test_ecdh(void *ctx, const uint8_t peer[DH_P256_PUBLIC_SIZE],
                      uint8_t out[DH_P256_SHARED_SIZE]) {
    (void)ctx;
    return dh_p256_ecdh(helper_private, peer, out);
}

/*
 * Scripted entropy. Draws that are set up are handed back in order; anything
 * beyond them is filled with a *changing* pattern rather than a constant, so a
 * test that under-scripts still gets distinct correlation values and cannot
 * pass because two of them happened to be equal.
 */
#define MAX_DRAWS 8
static uint8_t scripted[MAX_DRAWS][DH_NONCE_SIZE];
static size_t scripted_len[MAX_DRAWS];
static size_t scripted_count;
static size_t scripted_next;
static uint8_t filler_seed;

static void reset_entropy(void) {
    scripted_count = 0;
    scripted_next = 0;
    filler_seed = 0;
}

static void script_draw(const uint8_t *bytes, size_t len) {
    if (scripted_count >= MAX_DRAWS || len > DH_NONCE_SIZE) return;
    memcpy(scripted[scripted_count], bytes, len);
    scripted_len[scripted_count++] = len;
}

static void test_entropy(void *ctx, uint8_t *out, size_t len) {
    (void)ctx;
    if (scripted_next < scripted_count && scripted_len[scripted_next] == len) {
        memcpy(out, scripted[scripted_next++], len);
        return;
    }
    filler_seed++;
    for (size_t i = 0; i < len; i++) out[i] = (uint8_t)(filler_seed * 31u + i);
}

/* An enclave that will not answer, or a stored board key that is not a point
   on the curve. Both reach the helper as one ECDH returning false. */
static bool refusing_ecdh(void *ctx, const uint8_t peer[DH_P256_PUBLIC_SIZE],
                          uint8_t out[DH_P256_SHARED_SIZE]) {
    (void)ctx;
    (void)peer;
    (void)out;
    return false;
}

static dh_helper_identity identity;

static void an_identity(void) {
    memset(&identity, 0, sizeof identity);
    memcpy(identity.public_key, helper_public, DH_P256_PUBLIC_SIZE);
    memcpy(identity.key_id, helper_key_id, DH_KEY_ID_SIZE);
    identity.os = DH_OS_MAC;
    identity.build_type = DH_BUILD_RELEASE;
    identity.ecdh = test_ecdh;
    identity.entropy = test_entropy;
}

/* -------------------------------------------------------------- the far end */

static dh_session board;
static dh_pair pairing;

static void a_paired_board(void) {
    dh_session_init(&board, DH_BUILD_RELEASE);
    dh_session_stage_nonce(&board, published_board_nonce);
    dh_pair_init(&pairing);
    (void)dh_pair_set_identity(&pairing, board_private);
    dh_pair_set_registration(&pairing, helper_key_id, shared_secret);
}

static void an_unpaired_board(void) {
    dh_session_init(&board, DH_BUILD_RELEASE);
    dh_session_stage_nonce(&board, published_board_nonce);
    dh_pair_init(&pairing);
    (void)dh_pair_set_identity(&pairing, board_private);
}

/* The board's last reply, kept so a test can send it a second time. */
static uint8_t last_board_frame[DH_SESSION_REPLY_MAX];
static size_t last_board_frame_len;

/* One frame to the board; its reply, if any, straight back to the helper. */
static void relay_to_board(dh_helper *h, const uint8_t *frame, size_t len, uint32_t now_ms,
                           dh_helper_outputs *o) {
    if (dh_session_needs_nonce(&board)) dh_session_stage_nonce(&board, published_board_nonce);

    dh_frame_view v;
    size_t consumed = 0;
    if (dh_frame_decode(frame, len, &v, &consumed) != DH_FRAME_OK) return;

    uint8_t reply[DH_SESSION_REPLY_MAX];
    size_t reply_len = 0;
    if (dh_session_on_frame(&board, &pairing, &v, now_ms, reply, sizeof reply, &reply_len) !=
        DH_FRAME_OK)
        return;
    if (reply_len == 0) return;

    memcpy(last_board_frame, reply, reply_len);
    last_board_frame_len = reply_len;
    dh_helper_received(h, reply, reply_len, now_ms, o);
}

/* The board's own clock, so a long run is a real one: without its beats the
   helper is timing a session nobody is holding up. */
static void board_ticks(dh_helper *h, uint32_t now_ms, dh_helper_outputs *o) {
    uint8_t frame[DH_SESSION_REPLY_MAX];
    size_t len = 0;
    if (dh_session_tick(&board, now_ms, frame, sizeof frame, &len) != DH_FRAME_OK) return;
    if (len == 0) return;
    dh_session_note_owed_sent(&board, frame[0]);
    dh_helper_received(h, frame, len, now_ms, o);
}

/* Every frame the helper produced in `from`, answered by the board. */
static void answer_all(dh_helper *h, const dh_helper_outputs *from, uint32_t now_ms,
                       dh_helper_outputs *o) {
    for (size_t i = 0; i < from->count; i++)
        if (from->items[i].kind == DH_HELPER_OUT_SEND)
            relay_to_board(h, from->items[i].bytes, from->items[i].len, now_ms, o);
}

/*
 * One millisecond of both ends: the board's clock, the helper's clock, and
 * everything the helper produced carried over to the board.
 *
 * The last part is not decoration. A helper whose beats never reach the board
 * is evicted for silence inside three seconds, so a long run without it
 * measures the eviction rather than whatever the test was written to measure.
 */
static void pump(dh_helper *h, uint32_t now_ms, dh_helper_outputs *o) {
    board_ticks(h, now_ms, o);
    dh_helper_tick(h, now_ms, o);
    dh_helper_outputs produced = *o;
    answer_all(h, &produced, now_ms, o);
}

/*
 * A device→helper frame under the *published* k_b2h, at the next counter in
 * that space. One counter for the whole file, never reset: a receiver refuses
 * anything not strictly greater, and a counter that only rises is accepted
 * across a fresh handshake as readily as within one.
 *
 * Deliberately not mixed with `pump` in a single test. That drives the real
 * `dh_session`, which writes into this same counter space under the same key —
 * two writers, and the helper would refuse whichever frame lost the race.
 */
static uint64_t board_counter = 1;

/*
 * The published helper nonce, armed again. A second handshake in one test
 * draws a fresh nonce, so without this the session keys it derives are not the
 * published ones and `board_frame` builds something the helper is right to
 * refuse.
 */
static void republish_the_helper_nonce(void) {
    reset_entropy();
    script_draw(published_helper_nonce, DH_NONCE_SIZE);
}

static bool board_frame(uint8_t type, const uint8_t *body, size_t body_len, uint8_t *out,
                        size_t cap, size_t *out_len) {
    return dh_auth_frame(type, 0, k_b2h, board_counter++, body, body_len, out, cap, out_len) ==
           DH_FRAME_OK;
}

/* ------------------------------------------------------------ output digging */

static dh_helper_outputs out;

static const dh_helper_output *first_of(const dh_helper_outputs *o, dh_helper_output_kind kind) {
    for (size_t i = 0; i < o->count; i++)
        if (o->items[i].kind == kind) return &o->items[i];
    return NULL;
}

static size_t count_of(const dh_helper_outputs *o, dh_helper_output_kind kind) {
    size_t n = 0;
    for (size_t i = 0; i < o->count; i++)
        if (o->items[i].kind == kind) n++;
    return n;
}

static bool saw_state(const dh_helper_outputs *o, dh_helper_state state) {
    for (size_t i = 0; i < o->count; i++)
        if (o->items[i].kind == DH_HELPER_OUT_STATE && o->items[i].state == state) return true;
    return false;
}

static bool saw_note(const dh_helper_outputs *o, dh_helper_note note) {
    for (size_t i = 0; i < o->count; i++)
        if (o->items[i].kind == DH_HELPER_OUT_NOTE && o->items[i].note == note) return true;
    return false;
}

/* Nothing may be silently lost: an output that did not fit would look exactly
   like nothing having happened, which is the failure this whole file exists to
   catch elsewhere. Checked after every scenario. */
static void no_overflow(const char *name) {
    CHECK(out.overflow == 0, name, "outputs overflowed");
}

/* ------------------------------------------------------------- the fixtures */

/*
 * A helper holding the board's pinned key, with the channel open and the hello
 * sent, at t = 0.
 *
 * The published helper nonce is scripted in, and the board stages the
 * published board nonce, so the session keys this handshake derives are the
 * published ones. That is what lets a test build a device→helper frame under
 * the published k_b2h and have the helper accept it.
 */
static void a_helper_with_the_hello_sent(dh_helper *h) {
    an_identity();
    a_paired_board();
    reset_entropy();
    script_draw(published_helper_nonce, DH_NONCE_SIZE);
    dh_helper_init(h, &identity, board_public);

    dh_helper_outputs_reset(&out);
    dh_helper_device_appeared(h, DH_DEVICE_NORMAL, 0, &out);
    dh_helper_channels_acquired(h, 1, 0, &out);
}

/* The same, carried through the board's answer into a live session. */
static void a_live_session(dh_helper *h) {
    a_helper_with_the_hello_sent(h);

    dh_helper_outputs acquired = out;
    dh_helper_outputs_reset(&out);
    answer_all(h, &acquired, 0, &out);
}

/* ------------------------------------------------------------------- tests */

/*
 * The hello, byte for byte against the golden frame. The published nonce and
 * the golden frame's own correlation value are scripted in, so everything else
 * — the version, the negotiated asks, the key id, and the tag over all of it —
 * has to come out of this file's own encoding.
 */
static void test_the_hello_matches_the_golden_frame(void) {
    const char *name = "the hello matches the golden frame";
    const struct vector *golden = find("hello_mac");
    if (golden == NULL) return;

    /* The correlation sits in the body, which starts behind the header and the
       authentication prefix, after two version bytes, os, build, channel count
       and chunk size. */
    const size_t correlation_at = DH_FRAME_HEADER_SIZE + DH_FRAME_AUTH_PREFIX_SIZE + 7;

    an_identity();
    reset_entropy();
    script_draw(published_helper_nonce, DH_NONCE_SIZE);
    script_draw(golden->f[0] + correlation_at, 8);

    dh_helper h;
    dh_helper_init(&h, &identity, board_public);
    dh_helper_outputs_reset(&out);
    dh_helper_device_appeared(&h, DH_DEVICE_NORMAL, 0, &out);
    dh_helper_channels_acquired(&h, 1, 0, &out);

    const dh_helper_output *sent = first_of(&out, DH_HELPER_OUT_SEND);
    CHECK(sent != NULL, name, "no hello was sent");
    if (sent == NULL) return;
    CHECK(sent->len == golden->len[0], name, "the hello is the wrong length");
    CHECK(sent->len == golden->len[0] && memcmp(sent->bytes, golden->f[0], sent->len) == 0, name,
          "the hello does not match the golden frame");
    no_overflow(name);
}

/*
 * The whole handshake against the real board, and then the thing the ACs
 * single out: the effective numbers come from the *reply*, never from the
 * constants the hello asked with. Proved by an ack carrying numbers the board
 * would never choose.
 */
static void test_negotiation_comes_from_the_reply(void) {
    const char *name = "negotiation comes from the reply";
    dh_helper h;
    a_live_session(&h);

    CHECK(h.state == DH_HELPER_CONNECTED, name, "a live session is not reported as connected");
    CHECK(dh_helper_can_send_bulk(&h), name, "a live session refuses bulk");
    CHECK(h.have_negotiated, name, "nothing was negotiated");
    CHECK(h.negotiated.channel_count == DH_SESSION_CHANNEL_COUNT, name,
          "the board's channel count was not taken");
    no_overflow(name);

    /* Again, with an ack this file writes: three channels and a 256-byte
       chunk, neither of which is a constant in the core. */
    a_helper_with_the_hello_sent(&h);
    dh_hello_ack ack = {
        .correlation = h.hello_correlation,
        .proto_version = DH_PROTO_VERSION,
        .build_type = DH_BUILD_DEVELOPMENT,
        .channel_count = 3,
        .max_chunk = 256,
    };
    memcpy(ack.board_nonce, published_board_nonce, DH_NONCE_SIZE);

    uint8_t frame[DH_FRAME_MAX_SIZE];
    size_t len = 0;
    CHECK(dh_hello_ack_encode(&ack, k_b2h, 0, frame, sizeof frame, &len) == DH_FRAME_OK, name,
          "the ack would not encode");

    dh_helper_outputs_reset(&out);
    dh_helper_received(&h, frame, len, 0, &out);

    CHECK(h.negotiated.channel_count == 3, name, "the channel count was not read off the reply");
    CHECK(h.negotiated.max_chunk == 256, name, "the chunk size was not read off the reply");
    CHECK(saw_note(&out, DH_NOTE_DEVELOPMENT_BUILD), name,
          "a development board was not called out");
    no_overflow(name);
}

/*
 * ADR-0004. The beat fills a direction that has been idle for a full interval,
 * and *any* traffic in that direction suppresses it — including a frame the
 * platform sent that this machine never produced, which is what a bulk
 * transfer is.
 */
static void test_the_beat_only_fills_an_idle_direction(void) {
    const char *name = "the beat only fills an idle direction";
    dh_helper h;
    a_live_session(&h);

    dh_helper_outputs_reset(&out);
    dh_helper_tick(&h, DH_SESSION_HEARTBEAT_MS - 1, &out);
    CHECK(count_of(&out, DH_HELPER_OUT_SEND) == 0, name, "beat before the interval was up");

    dh_helper_outputs_reset(&out);
    dh_helper_tick(&h, DH_SESSION_HEARTBEAT_MS, &out);
    CHECK(count_of(&out, DH_HELPER_OUT_SEND) == 1, name, "no beat after a full idle interval");

    /* A transfer going out under the machine's feet. The next interval is
       measured from that, not from the last beat. */
    dh_helper_note_sent(&h, DH_SESSION_HEARTBEAT_MS + 500);
    dh_helper_outputs_reset(&out);
    dh_helper_tick(&h, 2 * DH_SESSION_HEARTBEAT_MS, &out);
    CHECK(count_of(&out, DH_HELPER_OUT_SEND) == 0, name, "beat into a direction carrying traffic");

    dh_helper_outputs_reset(&out);
    dh_helper_tick(&h, DH_SESSION_HEARTBEAT_MS + 500 + DH_SESSION_HEARTBEAT_MS, &out);
    CHECK(count_of(&out, DH_HELPER_OUT_SEND) == 1, name, "the beat did not resume when idle");
    no_overflow(name);
}

/* The board is absent only after the full window with nothing that
   authenticates, and any frame that does refreshes it. */
static void test_the_board_is_absent_only_after_the_window(void) {
    const char *name = "the board is absent only after the window";
    dh_helper h;
    a_live_session(&h);

    dh_helper_outputs_reset(&out);
    dh_helper_tick(&h, DH_SESSION_ABSENT_MS - 1, &out);
    CHECK(h.state == DH_HELPER_CONNECTED, name, "gave up inside the window");

    dh_helper_outputs_reset(&out);
    dh_helper_tick(&h, DH_SESSION_ABSENT_MS, &out);
    CHECK(saw_note(&out, DH_NOTE_DEVICE_SILENT), name, "silence was not noticed");
    CHECK(count_of(&out, DH_HELPER_OUT_CLOSE_CHANNELS) == 1, name, "the channels were not released");
    CHECK(first_of(&out, DH_HELPER_OUT_RETRY) != NULL, name, "no retry was asked for");
    no_overflow(name);

    /* The same clock, with one beat from the board part way through. */
    a_live_session(&h);
    uint8_t beat[DH_FRAME_MAX_SIZE];
    size_t beat_len = 0;
    CHECK(dh_auth_frame(DH_MSG_DEVICE_HEARTBEAT, 0, k_b2h, 1, NULL, 0, beat, sizeof beat,
                        &beat_len) == DH_FRAME_OK,
          name, "the beat would not encode");

    dh_helper_outputs_reset(&out);
    dh_helper_received(&h, beat, beat_len, 2000, &out);
    CHECK(count_of(&out, DH_HELPER_OUT_CLOSE_CHANNELS) == 0, name, "the beat was not accepted");
    dh_helper_tick(&h, DH_SESSION_ABSENT_MS, &out);
    CHECK(dh_helper_can_send_bulk(&h), name, "a beat did not refresh the deadline");
    no_overflow(name);
}

/*
 * A refused hello is not session traffic, and does not refresh liveness — v2
 * narrowed that deliberately, because under v1 anything writing into the
 * shared endpoint could hold a dead session open (#95, ADR-0008).
 *
 * What keeps an unpaired helper alive instead is that it has no session to
 * time out: the phase is live, nothing is negotiated, and the liveness
 * deadline is scoped to a negotiated session. So it can sit indefinitely being
 * told nothing, which is exactly what it must do while it waits for a chord.
 */
static void test_an_unpaired_helper_is_told_so_and_waits(void) {
    const char *name = "an unpaired helper is told so and waits";
    an_identity();
    an_unpaired_board();
    reset_entropy();

    dh_helper h;
    dh_helper_init(&h, &identity, NULL);
    dh_helper_outputs_reset(&out);
    dh_helper_device_appeared(&h, DH_DEVICE_NORMAL, 0, &out);
    dh_helper_channels_acquired(&h, 1, 0, &out);

    dh_helper_outputs acquired = out;
    dh_helper_outputs_reset(&out);
    answer_all(&h, &acquired, 0, &out);

    CHECK(saw_state(&out, DH_HELPER_NOT_PAIRED), name, "the refusal did not reach not-paired");
    CHECK(dh_helper_prompts_config_chord(h.state), name, "the chord was not offered");
    no_overflow(name);

    /* Nothing further arrives, for a long time. It must not give up. */
    for (uint32_t t = 1000; t <= 60000; t += 1000) {
        dh_helper_outputs_reset(&out);
        dh_helper_tick(&h, t, &out);
        no_overflow(name);
    }
    CHECK(h.state == DH_HELPER_NOT_PAIRED, name, "an unpaired helper timed itself out");
    CHECK(!dh_helper_can_send_bulk(&h), name, "an unpaired helper offered to carry bulk");
}

/* Acquisition is all or nothing, and a refused open names no remedy of its
   own — it is reported as an unusable device, once, after the silence
   window. */
static void test_a_refused_open_is_an_unusable_device(void) {
    const char *name = "a refused open is an unusable device";
    an_identity();
    reset_entropy();

    dh_helper h;
    dh_helper_init(&h, &identity, board_public);
    dh_helper_outputs_reset(&out);
    dh_helper_device_appeared(&h, DH_DEVICE_NORMAL, 0, &out);
    dh_helper_acquisition_refused(&h, 1, 2, 0, &out);

    CHECK(saw_note(&out, DH_NOTE_PARTIAL_ACQUISITION), name,
          "a partial acquisition was not reported as one");
    CHECK(count_of(&out, DH_HELPER_OUT_CLOSE_CHANNELS) == 1, name, "the part-held channel was kept");
    CHECK(count_of(&out, DH_HELPER_OUT_STATE) == 0, name, "a momentary refusal was reported");
    CHECK(!dh_helper_can_send_bulk(&h), name, "a refused open left bulk allowed");

    /*
     * It keeps failing. The deferral must not be pushed out by the later
     * refusals: the backoff caps at 4 s and the window is 5 s, so re-arming it
     * each time would move the deadline further away than the retries are
     * apart and the report would never come due.
     */
    bool said_absent = false;
    for (uint32_t t = 1000; t <= DH_HELPER_SILENCE_MS; t += 1000) {
        dh_helper_outputs_reset(&out);
        dh_helper_acquisition_refused(&h, 0, 2, t, &out);
        dh_helper_tick(&h, t, &out);
        if (saw_state(&out, DH_HELPER_DEVICE_ABSENT)) said_absent = true;
        no_overflow(name);
    }
    CHECK(said_absent, name, "a device that could never be opened said nothing");
}

/* A retry that completes clears the deferral, so the ordinary shape — channel
   nodes arriving one at a time — is silent. */
static void test_a_partial_acquisition_that_completes_is_silent(void) {
    const char *name = "a partial acquisition that completes is silent";
    an_identity();
    a_paired_board();
    reset_entropy();

    dh_helper h;
    dh_helper_init(&h, &identity, board_public);
    dh_helper_outputs_reset(&out);
    dh_helper_device_appeared(&h, DH_DEVICE_NORMAL, 0, &out);
    dh_helper_acquisition_refused(&h, 1, 2, 0, &out);
    dh_helper_channels_acquired(&h, 2, 500, &out);

    dh_helper_outputs acquired = out;
    dh_helper_outputs_reset(&out);
    answer_all(&h, &acquired, 500, &out);
    CHECK(h.state == DH_HELPER_CONNECTED, name, "the session did not come up");

    bool said_absent = false;
    for (uint32_t t = 1000; t <= 2 * DH_HELPER_SILENCE_MS; t += 500) {
        dh_helper_outputs_reset(&out);
        pump(&h, t, &out);
        if (saw_state(&out, DH_HELPER_DEVICE_ABSENT)) said_absent = true;
        no_overflow(name);
    }
    CHECK(!said_absent, name, "the deferral survived a good session");
    CHECK(h.state == DH_HELPER_CONNECTED, name, "the session did not hold");
}

/*
 * #73, and the shape of its fix (61e9127). The flag is keyed on *any* identity
 * appearing, not on the normal one: a helper started while the board is in
 * config mode would otherwise leave the never-attached fallback armed and
 * report "device not connected" five seconds in, while the user is looking at
 * the config page they opened on purpose.
 */
static void test_a_cold_start_in_config_mode_says_config_mode(void) {
    const char *name = "a cold start in config mode says config mode";
    an_identity();
    reset_entropy();

    dh_helper h;
    dh_helper_init(&h, &identity, board_public);
    dh_helper_outputs_reset(&out);
    dh_helper_device_appeared(&h, DH_DEVICE_CONFIG_MODE, 0, &out);

    bool said_absent = false;
    for (uint32_t t = 1000; t <= 3 * DH_HELPER_SILENCE_MS; t += 1000) {
        dh_helper_outputs_reset(&out);
        dh_helper_tick(&h, t, &out);
        if (saw_state(&out, DH_HELPER_DEVICE_ABSENT)) said_absent = true;
        no_overflow(name);
    }

    CHECK(h.state == DH_HELPER_DEVICE_IN_CONFIG_MODE, name, "config mode was not reported");
    CHECK(!said_absent, name, "a board in config mode was called absent");
    CHECK(!dh_helper_prompts_config_chord(h.state), name,
          "the chord was offered while the board was in config mode");

    /* It comes back under its own identity. */
    dh_helper_outputs_reset(&out);
    dh_helper_device_appeared(&h, DH_DEVICE_NORMAL, 20000, &out);
    CHECK(saw_state(&out, DH_HELPER_QUIET), name, "the stale message was not cleared");
    CHECK(first_of(&out, DH_HELPER_OUT_OPEN_CHANNELS) != NULL, name, "the channel was not reopened");
    no_overflow(name);
}

/* Nothing has ever attached — the other half of the same reporting, and the
   one the config-mode flag must not swallow. */
static void test_a_helper_that_never_sees_a_device_says_so(void) {
    const char *name = "a helper that never sees a device says so";
    an_identity();
    reset_entropy();

    dh_helper h;
    dh_helper_init(&h, &identity, board_public);
    dh_helper_outputs_reset(&out);
    dh_helper_tick(&h, 0, &out);
    dh_helper_tick(&h, DH_HELPER_SILENCE_MS - 1, &out);
    CHECK(!saw_state(&out, DH_HELPER_DEVICE_ABSENT), name, "reported inside the silence window");

    dh_helper_tick(&h, DH_HELPER_SILENCE_MS, &out);
    CHECK(saw_state(&out, DH_HELPER_DEVICE_ABSENT), name, "never reported at all");
    no_overflow(name);
}

/* Ordinary USB noise. A device that disappears for a moment says nothing. */
static void test_a_brief_disappearance_is_silent(void) {
    const char *name = "a brief disappearance is silent";
    dh_helper h;
    a_live_session(&h);

    dh_helper_outputs_reset(&out);
    dh_helper_device_disappeared(&h, 1000, &out);
    dh_helper_tick(&h, 2000, &out);
    dh_helper_device_appeared(&h, DH_DEVICE_NORMAL, 2500, &out);
    dh_helper_tick(&h, 8000, &out);

    CHECK(!saw_state(&out, DH_HELPER_DEVICE_ABSENT), name, "a blink was reported as an absence");
    CHECK(count_of(&out, DH_HELPER_OUT_CLOSE_CHANNELS) == 1, name, "the channels were not released");
    CHECK(count_of(&out, DH_HELPER_OUT_OPEN_CHANNELS) == 1, name, "the channels were not reopened");
    no_overflow(name);
}

/* The backoff doubles, caps, and starts short again after a session that
   worked. */
static void test_the_backoff_caps_and_resets(void) {
    const char *name = "the backoff caps and resets";
    an_identity();
    a_paired_board();
    reset_entropy();

    dh_helper h;
    dh_helper_init(&h, &identity, board_public);

    const uint32_t expected[] = {250, 500, 1000, 2000, 4000, 4000};
    for (size_t i = 0; i < sizeof expected / sizeof expected[0]; i++) {
        dh_helper_outputs_reset(&out);
        dh_helper_channels_acquired(&h, 1, (uint32_t)(i * 10000), &out);
        dh_helper_transport_failed(&h, (uint32_t)(i * 10000) + 1, &out);
        const dh_helper_output *retry = first_of(&out, DH_HELPER_OUT_RETRY);
        CHECK(retry != NULL && (uint32_t)retry->a == expected[i], name,
              "the backoff is not the doubling, capped one");
    }

    /* A session that comes up puts it back to the start. */
    dh_helper_outputs_reset(&out);
    dh_helper_channels_acquired(&h, 1, 100000, &out);
    dh_helper_outputs acquired = out;
    dh_helper_outputs_reset(&out);
    answer_all(&h, &acquired, 100000, &out);

    dh_helper_outputs_reset(&out);
    dh_helper_transport_failed(&h, 100001, &out);
    const dh_helper_output *retry = first_of(&out, DH_HELPER_OUT_RETRY);
    CHECK(retry != NULL && retry->a == 250, name, "a working session did not reset the backoff");
    no_overflow(name);
}

/*
 * The rate #94 cost two days. Each cycle on its own is correctly too brief to
 * report; four inside thirty seconds is not, and the number goes with it.
 */
static void test_a_flapping_link_is_reported_as_a_rate(void) {
    const char *name = "a flapping link is reported as a rate";
    dh_helper h;
    a_live_session(&h);

    bool rate_reported = false;
    for (uint32_t t = 1000; t <= 4000; t += 1000) {
        dh_helper_outputs_reset(&out);
        dh_helper_transport_failed(&h, t, &out);
        no_overflow(name);
        if (saw_note(&out, DH_NOTE_RECONNECTION_RATE)) rate_reported = true;

        dh_helper_outputs_reset(&out);
        dh_helper_channels_acquired(&h, 1, t + 100, &out);
        dh_helper_outputs acquired = out;
        dh_helper_outputs_reset(&out);
        answer_all(&h, &acquired, t + 100, &out);
        if (saw_note(&out, DH_NOTE_RECONNECTION_RATE)) rate_reported = true;
        no_overflow(name);
    }
    CHECK(h.state == DH_HELPER_RECONNECTING_REPEATEDLY, name, "the rate was not reported");
    CHECK(rate_reported, name, "the rate was reported without its number");
    CHECK(dh_helper_allows_bulk(h.state), name, "a rebuilt session was refused bulk");
    CHECK(!dh_helper_prompts_config_chord(h.state), name, "a flapping link offered the chord");
    CHECK(dh_helper_can_send_bulk(&h), name, "a rebuilt session is still a session");

    /* It ages out: the link holds, and the reading stops being true. */
    for (uint32_t t = 5000; t <= 40000; t += 1000) {
        dh_helper_outputs_reset(&out);
        /* The board beats too: the point is the rate ageing out, not a silence
           timeout arriving first because nobody was holding the session up. */
        pump(&h, t, &out);
        no_overflow(name);
    }
    CHECK(h.state == DH_HELPER_CONNECTED, name, "the rate never aged out");
}

/*
 * The whole pairing exchange against the real board, ending in a session. The
 * grant is what the pin comes from, and the helper stores it only once the key
 * has produced a hello.
 */
static void test_pairing_round_trip(void) {
    const char *name = "pairing round trip";
    an_identity();
    an_unpaired_board();
    reset_entropy();

    dh_helper h;
    dh_helper_init(&h, &identity, NULL);
    dh_helper_outputs_reset(&out);
    dh_helper_device_appeared(&h, DH_DEVICE_NORMAL, 0, &out);
    dh_helper_channels_acquired(&h, 1, 0, &out);

    dh_helper_outputs acquired = out;
    dh_helper_outputs_reset(&out);
    answer_all(&h, &acquired, 0, &out);
    CHECK(h.state == DH_HELPER_NOT_PAIRED, name, "the board did not say it was unpaired");

    /* The user presses the chord. */
    dh_pair_open_window(&pairing, 1000);

    dh_helper_outputs_reset(&out);
    dh_helper_tick(&h, 1000, &out);
    CHECK(count_of(&out, DH_HELPER_OUT_SEND) == 1, name, "no pairing request went out");

    dh_helper_outputs asked = out;
    dh_helper_outputs_reset(&out);
    answer_all(&h, &asked, 1000, &out);

    const dh_helper_output *stored = first_of(&out, DH_HELPER_OUT_STORE_BOARD_KEY);
    CHECK(stored != NULL, name, "the board key was not handed over for storage");
    CHECK(stored != NULL && stored->len == DH_P256_PUBLIC_SIZE &&
              memcmp(stored->bytes, board_public, DH_P256_PUBLIC_SIZE) == 0,
          name, "the stored key is not the board's");
    CHECK(saw_note(&out, DH_NOTE_PAIRED_BY_DEVICE), name, "pairing was not noted");

    /* The hello that followed the grant, answered. */
    dh_helper_outputs paired = out;
    dh_helper_outputs_reset(&out);
    answer_all(&h, &paired, 1000, &out);
    CHECK(h.state == DH_HELPER_CONNECTED, name, "pairing did not end in a session");
    no_overflow(name);
}

/*
 * A genuine grant, from something that is not the board this helper is paired
 * with. Accepting it silently is how a swapped board inherits the trust of the
 * one it replaced — and the chord must not be offered, because pressing it is
 * the act that would accept it (#112).
 */
static void test_a_board_whose_key_changed_is_not_accepted(void) {
    const char *name = "a board whose key changed is not accepted";
    an_identity();
    an_unpaired_board();
    reset_entropy();

    /* Pinned to somebody else: the published helper key, which is a valid
       point and is not this board. */
    dh_helper h;
    dh_helper_init(&h, &identity, helper_public);
    dh_helper_outputs_reset(&out);
    dh_helper_device_appeared(&h, DH_DEVICE_NORMAL, 0, &out);
    dh_helper_channels_acquired(&h, 1, 0, &out);

    dh_helper_outputs acquired = out;
    dh_helper_outputs_reset(&out);
    answer_all(&h, &acquired, 0, &out);

    dh_pair_open_window(&pairing, 1000);
    dh_helper_outputs_reset(&out);
    dh_helper_tick(&h, 1000, &out);
    dh_helper_outputs asked = out;
    dh_helper_outputs_reset(&out);
    answer_all(&h, &asked, 1000, &out);

    CHECK(h.state == DH_HELPER_BOARD_IDENTITY_CHANGED, name, "a different board was accepted");
    CHECK(first_of(&out, DH_HELPER_OUT_STORE_BOARD_KEY) == NULL, name,
          "the new board's key was stored anyway");
    CHECK(!dh_helper_prompts_config_chord(h.state), name,
          "the chord was offered for a swapped board");
    CHECK(memcmp(h.board_public_key, helper_public, DH_P256_PUBLIC_SIZE) == 0, name,
          "the pin was overwritten");
    no_overflow(name);
}

/*
 * The other half of #108. A listener cannot make the board lie, but it can
 * provoke a genuine refusal — which carries the *listener's* correlation
 * value. Believing one is how the "press the config chord" trap was
 * manufactured, so an answer to somebody else's question is dropped.
 */
static void test_an_answer_to_someone_elses_question_is_dropped(void) {
    const char *name = "an answer to someone else's question is dropped";
    dh_helper h;
    a_helper_with_the_hello_sent(&h);

    dh_hello_refused refused = {
        .correlation = h.hello_correlation ^ 1u,
        .proto_version = DH_PROTO_VERSION,
        .status = DH_HELLO_REFUSED_UNPAIRED,
    };
    uint8_t frame[DH_FRAME_MAX_SIZE];
    size_t len = 0;
    CHECK(dh_hello_refused_encode(&refused, frame, sizeof frame, &len) == DH_FRAME_OK, name,
          "the refusal would not encode");

    dh_helper_outputs_reset(&out);
    dh_helper_received(&h, frame, len, 100, &out);

    CHECK(h.state != DH_HELPER_NOT_PAIRED, name, "a provoked refusal reached the chord prompt");
    CHECK(saw_note(&out, DH_NOTE_IGNORED_WRONG_CORRELATION), name, "the mismatch was not noted");
    no_overflow(name);
}

/* A rate expires like one. The board says what it measured over a window; if
   nothing further arrives inside another such window, the warning goes. */
static void test_the_listener_alert_expires_like_a_rate(void) {
    const char *name = "the listener alert expires like a rate";
    dh_helper h;
    a_live_session(&h);

    uint8_t body[DH_LISTENER_ALERT_LEN];
    const uint32_t window = DH_LISTENER_WINDOW_MS;
    for (unsigned i = 0; i < 4; i++) body[i] = (uint8_t)(window >> (i * 8u));
    for (unsigned i = 0; i < 4; i++) body[4 + i] = (uint8_t)(9u >> (i * 8u));

    uint8_t frame[DH_FRAME_MAX_SIZE];
    size_t len = 0;
    CHECK(dh_auth_frame(DH_MSG_LISTENER_ALERT, 0, k_b2h, 1, body, sizeof body, frame, sizeof frame,
                        &len) == DH_FRAME_OK,
          name, "the alert would not encode");

    dh_helper_outputs_reset(&out);
    dh_helper_received(&h, frame, len, 1000, &out);
    CHECK(h.state == DH_HELPER_LISTENER_DETECTED, name, "the alert was not reported");
    CHECK(saw_note(&out, DH_NOTE_LISTENER_DETECTED), name, "the alert was reported without its rate");
    CHECK(dh_helper_allows_bulk(h.state), name,
          "a detected listener withheld bulk, disagreeing with the session");
    CHECK(!dh_helper_prompts_config_chord(h.state), name,
          "the chord was offered while something else was writing to the channel");

    /* Nothing further. The session must be kept alive, or the silence timeout
       arrives first and this measures the wrong thing. */
    for (uint32_t t = 2000; t <= 1000 + window; t += 500) {
        dh_helper_outputs_reset(&out);
        pump(&h, t, &out);
        no_overflow(name);
    }
    CHECK(h.state == DH_HELPER_CONNECTED, name, "the alert latched instead of expiring");
}

/*
 * A tag that does not verify means the board is not the board this helper
 * paired with, or the stream is corrupt — either way the connection goes, and
 * nothing is answered. A counter already seen is a different case: the tag
 * verified, so it came from the board, and the outbound queue drops frames as
 * ordinary business (ADR-0005). That costs the frame, not the session.
 */
static void test_a_bad_tag_drops_the_session_and_a_replay_does_not(void) {
    const char *name = "a bad tag drops the session and a replay does not";
    dh_helper h;
    a_live_session(&h);

    uint8_t bad[DH_FRAME_MAX_SIZE];
    size_t bad_len = 0;
    uint8_t wrong_key[DH_SESSION_KEY_SIZE];
    memset(wrong_key, 0x5A, sizeof wrong_key);
    CHECK(dh_auth_frame(DH_MSG_DEVICE_HEARTBEAT, 0, wrong_key, 1, NULL, 0, bad, sizeof bad,
                        &bad_len) == DH_FRAME_OK,
          name, "the frame would not encode");

    dh_helper_outputs_reset(&out);
    dh_helper_received(&h, bad, bad_len, 100, &out);
    CHECK(saw_note(&out, DH_NOTE_TAG_FAILED), name, "a failed tag was not noticed");
    CHECK(count_of(&out, DH_HELPER_OUT_CLOSE_CHANNELS) == 1, name, "a failed tag kept the session");
    CHECK(count_of(&out, DH_HELPER_OUT_SEND) == 0, name, "a failed tag was answered");
    no_overflow(name);

    /* A good frame, replayed. */
    a_live_session(&h);
    uint8_t beat[DH_FRAME_MAX_SIZE];
    size_t beat_len = 0;
    CHECK(dh_auth_frame(DH_MSG_DEVICE_HEARTBEAT, 0, k_b2h, 1, NULL, 0, beat, sizeof beat,
                        &beat_len) == DH_FRAME_OK,
          name, "the beat would not encode");

    dh_helper_outputs_reset(&out);
    dh_helper_received(&h, beat, beat_len, 100, &out);
    dh_helper_received(&h, beat, beat_len, 200, &out);
    CHECK(saw_note(&out, DH_NOTE_COUNTER_REPLAYED), name, "a replay was not noticed");
    CHECK(count_of(&out, DH_HELPER_OUT_CLOSE_CHANNELS) == 0, name, "a replay cost the session");
    CHECK(h.state == DH_HELPER_CONNECTED, name, "a replay changed what the user is told");
    no_overflow(name);
}

/* The board ending a session is acted on at once, rather than waited out. */
static void test_a_session_end_is_acted_on(void) {
    const char *name = "a session end is acted on";
    dh_helper h;
    a_live_session(&h);

    uint8_t body[DH_SESSION_END_LEN] = {DH_SESSION_END_UNPAIRED};
    uint8_t frame[DH_FRAME_MAX_SIZE];
    size_t len = 0;
    CHECK(dh_auth_frame(DH_MSG_SESSION_END, 0, k_b2h, 1, body, sizeof body, frame, sizeof frame,
                        &len) == DH_FRAME_OK,
          name, "the session end would not encode");

    dh_helper_outputs_reset(&out);
    dh_helper_received(&h, frame, len, 100, &out);

    const dh_helper_output *note = NULL;
    for (size_t i = 0; i < out.count; i++)
        if (out.items[i].kind == DH_HELPER_OUT_NOTE && out.items[i].note == DH_NOTE_SESSION_ENDED)
            note = &out.items[i];
    CHECK(note != NULL, name, "the end was not reported");
    CHECK(note != NULL && note->a == DH_SESSION_END_UNPAIRED, name, "the reason was lost");
    CHECK(!dh_helper_can_send_bulk(&h), name, "an ended session still carried bulk");
    no_overflow(name);
}

/*
 * The two predicates, over every state. They are the #34 security property and
 * the seam #52 consumes, so they are asserted as a table rather than left to a
 * reading of the enum — and the table is what a second platform must not get
 * to re-decide.
 */
static void test_the_policy_predicates_are_decided_once(void) {
    const char *name = "the policy predicates are decided once";
    const struct {
        dh_helper_state state;
        bool chord;
        bool bulk;
    } table[] = {
        {DH_HELPER_QUIET, false, false},
        {DH_HELPER_CONNECTED, false, true},
        {DH_HELPER_RECONNECTING_REPEATEDLY, false, true},
        {DH_HELPER_NOT_PAIRED, true, false},
        {DH_HELPER_DEVICE_IN_CONFIG_MODE, false, false},
        {DH_HELPER_DEVICE_ABSENT, false, false},
        {DH_HELPER_VERSION_INCOMPATIBLE, false, false},
        {DH_HELPER_LISTENER_DETECTED, false, true},
        {DH_HELPER_BOARD_IDENTITY_CHANGED, false, false},
    };

    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        CHECK(dh_helper_prompts_config_chord(table[i].state) == table[i].chord, name,
              "the chord is offered from the wrong state");
        CHECK(dh_helper_allows_bulk(table[i].state) == table[i].bulk, name,
              "bulk is allowed from the wrong state");
    }
}

/* An incompatible board keeps placement and refuses bulk: a misparsed
   placement self-corrects, a misparsed chunk header writes a corrupted file
   presented as valid. */
static void test_an_incompatible_board_refuses_bulk(void) {
    const char *name = "an incompatible board refuses bulk";
    dh_helper h;
    a_helper_with_the_hello_sent(&h);

    dh_hello_refused refused = {
        .correlation = h.hello_correlation,
        .proto_version = DH_PROTO_VERSION + 1u,
        .status = DH_HELLO_REFUSED_VERSION_INCOMPATIBLE,
    };
    uint8_t frame[DH_FRAME_MAX_SIZE];
    size_t len = 0;
    CHECK(dh_hello_refused_encode(&refused, frame, sizeof frame, &len) == DH_FRAME_OK, name,
          "the refusal would not encode");

    dh_helper_outputs_reset(&out);
    dh_helper_received(&h, frame, len, 100, &out);

    CHECK(h.state == DH_HELPER_VERSION_INCOMPATIBLE, name, "the mismatch was not reported");
    CHECK(saw_note(&out, DH_NOTE_VERSION_MISMATCH), name, "the versions were not recorded");
    CHECK(!dh_helper_allows_bulk(h.state), name, "an incompatible board was offered bulk");
    CHECK(!dh_helper_can_send_bulk(&h), name, "an incompatible board kept a session");
    no_overflow(name);
}

/* A hello with no answer is a dead session, not a slow one. */
static void test_an_unanswered_hello_is_a_dead_session(void) {
    const char *name = "an unanswered hello is a dead session";
    dh_helper h;
    a_helper_with_the_hello_sent(&h);

    dh_helper_outputs_reset(&out);
    dh_helper_tick(&h, DH_HELPER_HELLO_TIMEOUT_MS - 1, &out);
    CHECK(count_of(&out, DH_HELPER_OUT_CLOSE_CHANNELS) == 0, name, "gave up early");

    dh_helper_tick(&h, DH_HELPER_HELLO_TIMEOUT_MS, &out);
    CHECK(saw_note(&out, DH_NOTE_NO_ACK), name, "an unanswered hello was waited out for ever");
    CHECK(count_of(&out, DH_HELPER_OUT_CLOSE_CHANNELS) == 1, name, "the channels were kept");
    no_overflow(name);
}

/*
 * The clock wrapping. Every deadline here is an unsigned difference from when
 * something happened, never a stored `then + span`: near the end of the range
 * that sum is a small number while the clock is still a large one, so every
 * comparison against it reads as already past. A five-second window would come
 * due in ninety milliseconds, once every 49 days, and only on a board that had
 * been up that long.
 */
static void test_the_deadlines_survive_the_clock_wrapping(void) {
    const char *name = "the deadlines survive the clock wrapping";
    /* Close enough to the end that every window below crosses zero. */
    const uint32_t before_wrap = 0xFFFFFF00u;

    an_identity();
    a_paired_board();
    reset_entropy();

    dh_helper h;
    dh_helper_init(&h, &identity, board_public);
    dh_helper_outputs_reset(&out);
    dh_helper_device_appeared(&h, DH_DEVICE_NORMAL, before_wrap, &out);
    dh_helper_channels_acquired(&h, 1, before_wrap, &out);

    dh_helper_outputs acquired = out;
    dh_helper_outputs_reset(&out);
    answer_all(&h, &acquired, before_wrap, &out);
    CHECK(h.state == DH_HELPER_CONNECTED, name, "no session before the wrap");

    /* Liveness. Ninety milliseconds in, still before the wrap, the session is
       not silent — and the deadline it is measured against was set before the
       wrap too. */
    dh_helper_outputs_reset(&out);
    dh_helper_tick(&h, before_wrap + 90, &out);
    /* On the session, not on the state: a dropped connection leaves what the
       user is told alone until the silence window expires, so `connected` here
       would be true either way and could not tell the two apart. */
    CHECK(dh_helper_can_send_bulk(&h), name, "the session went silent 90 ms in");

    dh_helper_tick(&h, before_wrap + DH_SESSION_ABSENT_MS - 1, &out);
    CHECK(dh_helper_can_send_bulk(&h), name, "the wrap was read as silence");

    dh_helper_tick(&h, before_wrap + DH_SESSION_ABSENT_MS, &out);
    CHECK(saw_note(&out, DH_NOTE_DEVICE_SILENT), name, "silence past the wrap was never noticed");
    no_overflow(name);

    /* The deferral, which is the deadline most likely to be written as a sum.
       This one is armed before the wrap and comes due after it. */
    a_helper_with_the_hello_sent(&h);
    dh_helper_outputs sent = out;
    dh_helper_outputs_reset(&out);
    answer_all(&h, &sent, 0, &out);

    dh_helper_outputs_reset(&out);
    dh_helper_transport_failed(&h, before_wrap + 10, &out);

    dh_helper_outputs_reset(&out);
    dh_helper_tick(&h, before_wrap + 100, &out);
    CHECK(!saw_state(&out, DH_HELPER_DEVICE_ABSENT), name,
          "the deferral came due 90 ms into its five-second window");

    dh_helper_tick(&h, before_wrap + 10 + DH_HELPER_SILENCE_MS, &out);
    CHECK(saw_state(&out, DH_HELPER_DEVICE_ABSENT), name,
          "the deferral never came due across the wrap");
    no_overflow(name);
}

/*
 * A hello that cannot be built at all — the enclave will not answer, or the
 * stored board key is not a point on the curve. The device is unusable and
 * the user has to be told so, on the same terms as any other unusable device:
 * nothing at first, because a momentary failure is not worth reporting, and
 * "device not connected" once the silence window has passed.
 *
 * Saying nothing for ever is the outcome that has to be impossible here. It
 * is what a menu bar showing a working helper over a dead one looks like, and
 * it is the same shape as #94.
 */
static void test_a_hello_that_cannot_be_built_is_still_reported(void) {
    const char *name = "a hello that cannot be built is still reported";
    an_identity();
    identity.ecdh = refusing_ecdh;
    reset_entropy();

    dh_helper h;
    dh_helper_init(&h, &identity, board_public);
    dh_helper_outputs_reset(&out);
    dh_helper_device_appeared(&h, DH_DEVICE_NORMAL, 0, &out);
    dh_helper_channels_acquired(&h, 1, 0, &out);

    CHECK(saw_note(&out, DH_NOTE_KEY_DERIVATION_FAILED), name,
          "a key that cannot be used was not named as the reason");
    CHECK(count_of(&out, DH_HELPER_OUT_CLOSE_CHANNELS) == 1, name, "the channels were kept");
    CHECK(count_of(&out, DH_HELPER_OUT_SEND) == 0, name, "something went out anyway");

    bool said_absent = false;
    for (uint32_t t = 1000; t <= 2 * DH_HELPER_SILENCE_MS; t += 1000) {
        dh_helper_outputs_reset(&out);
        dh_helper_channels_acquired(&h, 1, t, &out);
        dh_helper_tick(&h, t, &out);
        if (saw_state(&out, DH_HELPER_DEVICE_ABSENT)) said_absent = true;
        no_overflow(name);
    }
    CHECK(said_absent, name, "a device that could never be used said nothing at all");
}

/*
 * A PAIR_GRANT nobody asked for. The correlation value is still the one this
 * helper used, because it is the same grant arriving twice — so correlation
 * cannot be what stops it, and what does is that no request is outstanding.
 *
 * Acting on it tears down the live session the first copy produced, which is
 * the opposite of what a pairing is for.
 */
static void test_a_grant_nobody_asked_for_is_ignored(void) {
    const char *name = "a grant nobody asked for is ignored";
    an_identity();
    an_unpaired_board();
    reset_entropy();

    dh_helper h;
    dh_helper_init(&h, &identity, NULL);
    dh_helper_outputs_reset(&out);
    dh_helper_device_appeared(&h, DH_DEVICE_NORMAL, 0, &out);
    dh_helper_channels_acquired(&h, 1, 0, &out);

    dh_helper_outputs acquired = out;
    dh_helper_outputs_reset(&out);
    answer_all(&h, &acquired, 0, &out);

    dh_pair_open_window(&pairing, 1000);
    dh_helper_outputs_reset(&out);
    dh_helper_tick(&h, 1000, &out);
    dh_helper_outputs asked = out;
    dh_helper_outputs_reset(&out);
    answer_all(&h, &asked, 1000, &out);

    /* The grant, kept aside before the hello that follows it overwrites it. */
    uint8_t grant[DH_SESSION_REPLY_MAX];
    size_t grant_len = last_board_frame_len;
    memcpy(grant, last_board_frame, grant_len);
    CHECK(grant_len > 0 && grant[0] == DH_MSG_PAIR_GRANT, name, "no grant was captured");

    dh_helper_outputs paired = out;
    dh_helper_outputs_reset(&out);
    answer_all(&h, &paired, 1000, &out);
    CHECK(h.state == DH_HELPER_CONNECTED, name, "pairing did not end in a session");

    /* The same grant again, 2 s into the session it produced. */
    dh_helper_outputs_reset(&out);
    dh_helper_received(&h, grant, grant_len, 3000, &out);

    CHECK(dh_helper_can_send_bulk(&h), name, "a repeated grant tore down the live session");
    CHECK(count_of(&out, DH_HELPER_OUT_STORE_BOARD_KEY) == 0, name,
          "a repeated grant asked for the key to be stored again");
    CHECK(count_of(&out, DH_HELPER_OUT_SEND) == 0, name, "a repeated grant sent a fresh hello");
    no_overflow(name);
}

/*
 * The seam #52 consumes. The counter space belongs to the key (dh_auth.h), so
 * the machine hands out the frames rather than the platform keeping a counter
 * of its own beside the heartbeat's — two writers in one counter space means
 * the board refuses whichever frame loses the race, and neither writer can
 * tell why.
 */
static void test_the_machine_owns_the_counter_bulk_goes_out_under(void) {
    const char *name = "the machine owns the counter bulk goes out under";
    dh_helper h;
    a_live_session(&h);

    /* Two frames in a row, then a beat, then another: every one must be
       accepted by the board, which refuses anything not strictly greater. */
    for (unsigned i = 0; i < 2; i++) {
        uint8_t frame[DH_FRAME_MAX_SIZE];
        size_t len = 0;
        const uint8_t body[4] = {1, 2, 3, (uint8_t)i};
        CHECK(dh_helper_emit(&h, DH_MSG_CLIP_CHUNK, 0, body, sizeof body, frame, sizeof frame,
                             &len) == DH_FRAME_OK,
              name, "a bulk frame would not be built");

        dh_frame_view v;
        size_t consumed = 0;
        CHECK(dh_frame_decode(frame, len, &v, &consumed) == DH_FRAME_OK, name,
              "the bulk frame would not decode");

        const uint8_t *out_body = NULL;
        size_t out_len = 0;
        CHECK(dh_session_authenticate(&board, &v, 100 + i, &out_body, &out_len) == DH_AUTH_OK, name,
              "the board refused a bulk frame this machine built");
        dh_helper_note_sent(&h, 100 + i);
    }

    /* The frame charged the idle timer, so no beat is owed yet. */
    dh_helper_outputs_reset(&out);
    dh_helper_tick(&h, 1000, &out);
    CHECK(count_of(&out, DH_HELPER_OUT_SEND) == 0, name, "beat into a direction carrying bulk");

    /* And the beat that does follow keeps the counter moving forwards. */
    dh_helper_outputs_reset(&out);
    dh_helper_tick(&h, 1101, &out);
    const dh_helper_output *beat = first_of(&out, DH_HELPER_OUT_SEND);
    CHECK(beat != NULL, name, "no beat once the direction went idle");
    if (beat != NULL) {
        dh_frame_view v;
        size_t consumed = 0;
        CHECK(dh_frame_decode(beat->bytes, beat->len, &v, &consumed) == DH_FRAME_OK, name,
              "the beat would not decode");
        const uint8_t *out_body = NULL;
        size_t out_len = 0;
        CHECK(dh_session_authenticate(&board, &v, 1101, &out_body, &out_len) == DH_AUTH_OK, name,
              "the beat reused a counter the bulk frames had spent");
    }
    no_overflow(name);

    /* No session, nothing to tag with, and nobody to send it to. */
    an_identity();
    reset_entropy();
    dh_helper idle;
    dh_helper_init(&idle, &identity, board_public);
    uint8_t frame[DH_FRAME_MAX_SIZE];
    size_t len = 0;
    CHECK(dh_helper_emit(&idle, DH_MSG_CLIP_CHUNK, 0, NULL, 0, frame, sizeof frame, &len) !=
              DH_FRAME_OK,
          name, "a frame was tagged with no session");
}

/*
 * ADR-0004 gates the device's beat on an idle direction, so beats stopping
 * during a transfer is the design working — and it looks identical in a log to
 * a device that has stalled. #88 had to tell those apart on hardware, and the
 * helper had no surface for it at all: the beat arrived and was dropped on the
 * floor.
 *
 * Traced at the edges rather than per beat. A line per arrival would be a line
 * a second, which is precisely the log that hid a live defect for two days
 * during that sitting (#94, #98).
 */
static void test_the_device_beat_is_traced_where_it_changes(void) {
    const char *name = "the device beat is traced where it changes";
    dh_helper h;
    a_live_session(&h);

    uint8_t frame[DH_FRAME_MAX_SIZE];
    size_t len = 0;

    /* The first beat says so, so "the beat never arrived" is distinguishable
       from "the beat was never worth mentioning". */
    dh_helper_outputs_reset(&out);
    CHECK(board_frame(DH_MSG_DEVICE_HEARTBEAT, NULL, 0, frame, sizeof frame, &len), name,
          "the beat would not encode");
    dh_helper_received(&h, frame, len, 0, &out);
    CHECK(saw_note(&out, DH_NOTE_FIRST_BEAT), name,
          "the first beat of a session was not traced as the first");

    /* On time, it says nothing. */
    uint32_t t = 0;
    for (unsigned i = 0; i < 5; i++) {
        t += DH_SESSION_HEARTBEAT_MS;
        dh_helper_outputs_reset(&out);
        CHECK(board_frame(DH_MSG_DEVICE_HEARTBEAT, NULL, 0, frame, sizeof frame, &len), name,
              "the beat would not encode");
        dh_helper_received(&h, frame, len, t, &out);
        CHECK(count_of(&out, DH_HELPER_OUT_NOTE) == 0, name, "a beat arriving on time was traced");
        no_overflow(name);
    }
    const uint32_t last_beat = t;

    /*
     * A transfer: the device keeps sending, so the session holds while the
     * idle-gated beat correctly stops. The placement frames are what keep
     * liveness up — they authenticate, so they are liveness, which is the
     * whole of ADR-0004.
     */
    const uint8_t place[4] = {1, 0, 0, 0x80};
    unsigned quiet_notes = 0;
    for (unsigned i = 0; i < 4; i++) {
        t += DH_SESSION_HEARTBEAT_MS;
        dh_helper_outputs_reset(&out);
        dh_helper_tick(&h, t, &out);
        if (saw_note(&out, DH_NOTE_BEAT_QUIET)) quiet_notes++;
        CHECK(board_frame(DH_MSG_PLACE, place, sizeof place, frame, sizeof frame, &len), name,
              "the placement would not encode");
        dh_helper_received(&h, frame, len, t, &out);
        no_overflow(name);
    }
    CHECK(h.state == DH_HELPER_CONNECTED, name, "a transfer without beats dropped the session");
    CHECK(quiet_notes == 1, name, "the beat falling silent was not traced exactly once");

    /* And the far side of it, with the measurement attached — a note that says
       a gap without saying how long it was is the note #94 cost two days to. */
    t += DH_SESSION_HEARTBEAT_MS;
    dh_helper_outputs_reset(&out);
    CHECK(board_frame(DH_MSG_DEVICE_HEARTBEAT, NULL, 0, frame, sizeof frame, &len), name,
          "the beat would not encode");
    dh_helper_received(&h, frame, len, t, &out);

    const dh_helper_output *resumed = NULL;
    for (size_t i = 0; i < out.count; i++)
        if (out.items[i].kind == DH_HELPER_OUT_NOTE && out.items[i].note == DH_NOTE_BEAT_RESUMED)
            resumed = &out.items[i];
    CHECK(resumed != NULL, name, "the beat coming back was not traced");
    CHECK(resumed != NULL && (uint32_t)resumed->a == t - last_beat, name,
          "the resumption was traced without saying how long the gap was");
    no_overflow(name);
}

/*
 * The trace is measured inside one session and must not outlive it.
 *
 * A config-mode round trip is the common path that proves it: the board leaves
 * under its other identity for minutes, which is `device_left` and not
 * `drop_connection`, and a beat remembered from before the chord makes both
 * edges of the next session wrong — a "quiet for 300.0s" about a session one
 * tick old, and a genuine first beat announcing itself as a resumption (#98).
 *
 * The tail of the old session is the other half: a beat still in the read queue
 * when the connection went used to announce the first beat of a session that
 * does not exist, and then swallow the real one.
 */
static void test_the_beat_trace_does_not_outlive_its_session(void) {
    const char *name = "the beat trace does not outlive its session";
    dh_helper h;
    a_live_session(&h);

    uint8_t frame[DH_FRAME_MAX_SIZE];
    size_t len = 0;
    dh_helper_outputs_reset(&out);
    CHECK(board_frame(DH_MSG_DEVICE_HEARTBEAT, NULL, 0, frame, sizeof frame, &len), name,
          "the beat would not encode");
    dh_helper_received(&h, frame, len, 0, &out);

    /* The chord: config mode, minutes away, then back as itself. */
    dh_helper_outputs_reset(&out);
    dh_helper_device_appeared(&h, DH_DEVICE_CONFIG_MODE, 1000, &out);
    dh_helper_tick(&h, 6000, &out);
    CHECK(h.state == DH_HELPER_DEVICE_IN_CONFIG_MODE, name, "config mode was not reported");

    republish_the_helper_nonce();
    dh_helper_outputs_reset(&out);
    dh_helper_device_appeared(&h, DH_DEVICE_NORMAL, 300000, &out);
    dh_helper_channels_acquired(&h, 1, 300000, &out);
    dh_helper_outputs acquired = out;
    dh_helper_outputs_reset(&out);
    answer_all(&h, &acquired, 300000, &out);
    CHECK(h.state == DH_HELPER_CONNECTED, name, "the session did not come back");

    dh_helper_outputs_reset(&out);
    dh_helper_tick(&h, 300250, &out);
    CHECK(!saw_note(&out, DH_NOTE_BEAT_QUIET), name,
          "a quiet spell measured before the chord outlived the session it belonged to");

    dh_helper_outputs_reset(&out);
    CHECK(board_frame(DH_MSG_DEVICE_HEARTBEAT, NULL, 0, frame, sizeof frame, &len), name,
          "the beat would not encode");
    dh_helper_received(&h, frame, len, 300250, &out);
    CHECK(saw_note(&out, DH_NOTE_FIRST_BEAT), name,
          "the first beat after a config-mode round trip was not traced as the first");
    no_overflow(name);

    /* And a beat that arrives after the connection has gone. Under v2 it is
       refused a step earlier, at the tag — the session key went with the
       session — but the property is unchanged: it must not consume the next
       session's first. */
    a_live_session(&h);
    uint8_t stray[DH_FRAME_MAX_SIZE];
    size_t stray_len = 0;
    CHECK(board_frame(DH_MSG_DEVICE_HEARTBEAT, NULL, 0, stray, sizeof stray, &stray_len), name,
          "the beat would not encode");

    dh_helper_outputs_reset(&out);
    dh_helper_transport_failed(&h, 100, &out);

    dh_helper_outputs_reset(&out);
    dh_helper_received(&h, stray, stray_len, 200, &out);
    CHECK(saw_note(&out, DH_NOTE_NO_SESSION_KEY), name,
          "a beat outside a session was acted on rather than refused");
    CHECK(!saw_note(&out, DH_NOTE_FIRST_BEAT), name,
          "a beat outside a session announced the first beat of one");

    republish_the_helper_nonce();
    dh_helper_outputs_reset(&out);
    dh_helper_channels_acquired(&h, 1, 300, &out);
    acquired = out;
    dh_helper_outputs_reset(&out);
    answer_all(&h, &acquired, 300, &out);

    dh_helper_outputs_reset(&out);
    CHECK(board_frame(DH_MSG_DEVICE_HEARTBEAT, NULL, 0, frame, sizeof frame, &len), name,
          "the beat would not encode");
    dh_helper_received(&h, frame, len, 400, &out);
    CHECK(saw_note(&out, DH_NOTE_FIRST_BEAT), name,
          "the stale beat consumed the new session's first");
    no_overflow(name);
}

/*
 * The same leak on the pairing path, which the machine reaches without ever
 * going idle. A HELLO_REFUSED(unpaired) ends the session while deliberately
 * keeping the phase live — #46 needs the helper up and asking — so it is the
 * one place where losing a session is not going quiet, and the one a teardown
 * keyed on the phase steps straight past.
 *
 * While it waits, it must trace nothing: the board holds no session for it and
 * so sends it nothing by design, and tracing the absence of beats that are not
 * supposed to exist is noise in the log the trace exists to keep readable.
 */
static void test_the_beat_trace_starts_afresh_after_a_hello_is_refused(void) {
    const char *name = "the beat trace starts afresh after a hello is refused";
    dh_helper h;
    a_live_session(&h);

    uint8_t frame[DH_FRAME_MAX_SIZE];
    size_t len = 0;
    dh_helper_outputs_reset(&out);
    CHECK(board_frame(DH_MSG_DEVICE_HEARTBEAT, NULL, 0, frame, sizeof frame, &len), name,
          "the beat would not encode");
    dh_helper_received(&h, frame, len, 0, &out);

    /* The link goes, and this time the board has forgotten the registration. */
    dh_helper_outputs_reset(&out);
    dh_helper_transport_failed(&h, 100, &out);
    dh_pair_init(&pairing);
    (void)dh_pair_set_identity(&pairing, board_private);
    dh_session_init(&board, DH_BUILD_RELEASE);
    dh_session_stage_nonce(&board, published_board_nonce);

    dh_helper_outputs_reset(&out);
    dh_helper_channels_acquired(&h, 1, 200, &out);
    dh_helper_outputs acquired = out;
    dh_helper_outputs_reset(&out);
    answer_all(&h, &acquired, 200, &out);
    CHECK(h.state == DH_HELPER_NOT_PAIRED, name, "the refused hello did not leave it unpaired");

    /* Nothing is traced while it waits. */
    for (uint32_t t = 1000; t <= 9000; t += 1000) {
        dh_helper_outputs_reset(&out);
        dh_helper_tick(&h, t, &out);
        CHECK(!saw_note(&out, DH_NOTE_BEAT_QUIET), name,
              "an unpaired helper traced beats the device is designed not to send it");
        no_overflow(name);
    }
    CHECK(h.state == DH_HELPER_NOT_PAIRED, name, "the unpaired helper was torn down");

    /* The chord lands. 11000 is where the next ask is due — the retry interval
       is 2 s and the last one went out at 9000, so a window opened at 10000
       would find nothing on the wire to grant. */
    dh_pair_open_window(&pairing, 11000);
    dh_helper_outputs_reset(&out);
    dh_helper_tick(&h, 11000, &out);
    CHECK(count_of(&out, DH_HELPER_OUT_SEND) == 1, name, "no pairing request went out");

    dh_helper_outputs asked = out;
    republish_the_helper_nonce();
    dh_helper_outputs_reset(&out);
    answer_all(&h, &asked, 11000, &out);
    dh_helper_outputs paired = out;
    CHECK(first_of(&paired, DH_HELPER_OUT_STORE_BOARD_KEY) != NULL, name,
          "the chord did not pin the board's key");

    dh_helper_outputs_reset(&out);
    answer_all(&h, &paired, 11000, &out);
    CHECK(h.state == DH_HELPER_CONNECTED, name, "pairing did not establish a session");

    dh_helper_outputs_reset(&out);
    CHECK(board_frame(DH_MSG_DEVICE_HEARTBEAT, NULL, 0, frame, sizeof frame, &len), name,
          "the beat would not encode");
    dh_helper_received(&h, frame, len, 11100, &out);
    CHECK(saw_note(&out, DH_NOTE_FIRST_BEAT), name,
          "the first beat of the session pairing established was not traced at all");
    no_overflow(name);
}

/*
 * The hello half of #108. An attacker who can write to the channel can produce
 * a well-formed, correctly tagged ack — what it cannot produce is the random
 * value this helper put in the question it is answering. A helper that acts on
 * any ack it can verify is one an unrelated conversation can walk into a
 * session with.
 */
static void test_an_ack_for_someone_elses_hello_is_dropped(void) {
    const char *name = "an ack for someone else's hello is dropped";
    dh_helper h;
    a_helper_with_the_hello_sent(&h);

    dh_hello_ack ack = {
        .correlation = h.hello_correlation ^ 1u,
        .proto_version = DH_PROTO_VERSION,
        .build_type = DH_BUILD_RELEASE,
        .channel_count = 1,
        .max_chunk = 256,
    };
    memcpy(ack.board_nonce, published_board_nonce, DH_NONCE_SIZE);

    uint8_t frame[DH_FRAME_MAX_SIZE];
    size_t len = 0;
    CHECK(dh_hello_ack_encode(&ack, k_b2h, 0, frame, sizeof frame, &len) == DH_FRAME_OK, name,
          "the ack would not encode");

    dh_helper_outputs_reset(&out);
    dh_helper_received(&h, frame, len, 100, &out);
    CHECK(saw_note(&out, DH_NOTE_IGNORED_WRONG_CORRELATION), name, "the mismatch was not noted");
    CHECK(h.state != DH_HELPER_CONNECTED, name, "an ack answering a different question connected");
    CHECK(!dh_helper_can_send_bulk(&h), name, "a session built on somebody else's ack carries bulk");
    no_overflow(name);

    /*
     * And the real answer still works. Dropping one must not poison the
     * handshake still legitimately in flight — nor spend the counter, which is
     * why the ack path verifies against a counter of its own and commits
     * nothing until it has.
     */
    ack.correlation = h.hello_correlation;
    CHECK(dh_hello_ack_encode(&ack, k_b2h, 0, frame, sizeof frame, &len) == DH_FRAME_OK, name,
          "the genuine ack would not encode");
    dh_helper_outputs_reset(&out);
    dh_helper_received(&h, frame, len, 100, &out);
    CHECK(h.state == DH_HELPER_CONNECTED, name,
          "the genuine ack was refused after a forged one had been dropped");
    no_overflow(name);
}

/*
 * The same trap on the pairing path, which is the one #108 was opened for: a
 * manufactured PAIR_GRANT arriving without any chord pins an attacker's key as
 * the board's. The correlation value in this helper's own PAIR_REQUEST is the
 * thing the attacker has to guess.
 *
 * The key offered is a real point on the curve, which ECDH would happily
 * accept. The correlation is the only thing wrong with the grant, and it has to
 * be enough on its own.
 */
static void test_a_grant_answering_someone_elses_request_is_dropped(void) {
    const char *name = "a grant answering someone else's request is dropped";
    an_identity();
    an_unpaired_board();
    reset_entropy();

    dh_helper h;
    dh_helper_init(&h, &identity, NULL);
    dh_helper_outputs_reset(&out);
    dh_helper_device_appeared(&h, DH_DEVICE_NORMAL, 0, &out);
    dh_helper_channels_acquired(&h, 1, 0, &out);

    dh_helper_outputs acquired = out;
    dh_helper_outputs_reset(&out);
    answer_all(&h, &acquired, 0, &out);
    CHECK(h.state == DH_HELPER_NOT_PAIRED, name, "the board did not say it was unpaired");

    dh_helper_outputs_reset(&out);
    dh_helper_tick(&h, 1000, &out);
    CHECK(count_of(&out, DH_HELPER_OUT_SEND) == 1, name, "no pairing request went out");

    dh_pair_grant grant = {.correlation = h.pair_correlation ^ 1u};
    memcpy(grant.board_public, helper_public, DH_P256_PUBLIC_SIZE);

    uint8_t frame[DH_FRAME_MAX_SIZE];
    size_t len = 0;
    CHECK(dh_pair_grant_encode(&grant, frame, sizeof frame, &len) == DH_FRAME_OK, name,
          "the grant would not encode");

    dh_helper_outputs_reset(&out);
    dh_helper_received(&h, frame, len, 1000, &out);
    CHECK(saw_note(&out, DH_NOTE_IGNORED_WRONG_CORRELATION), name, "the mismatch was not noted");
    CHECK(count_of(&out, DH_HELPER_OUT_STORE_BOARD_KEY) == 0, name,
          "a key nobody asked for was pinned as the board's");
    CHECK(count_of(&out, DH_HELPER_OUT_SEND) == 0, name, "a manufactured grant restarted the "
                                                          "handshake");
    CHECK(h.state == DH_HELPER_NOT_PAIRED, name, "a manufactured grant paired the helper");
    no_overflow(name);

    /* The board's own grant, echoing what this helper asked, is acted on. */
    grant.correlation = h.pair_correlation;
    memcpy(grant.board_public, board_public, DH_P256_PUBLIC_SIZE);
    CHECK(dh_pair_grant_encode(&grant, frame, sizeof frame, &len) == DH_FRAME_OK, name,
          "the genuine grant would not encode");

    dh_helper_outputs_reset(&out);
    dh_helper_received(&h, frame, len, 1000, &out);
    CHECK(first_of(&out, DH_HELPER_OUT_STORE_BOARD_KEY) != NULL, name,
          "the board's own grant was refused along with the forged one");
    CHECK(count_of(&out, DH_HELPER_OUT_SEND) == 1, name, "no fresh hello after being paired");
    no_overflow(name);
}

int main(int argc, char **argv) {
    const char *frames = argc > 1 ? argv[1] : DH_TEST_VECTORS;
    const char *primitives = argc > 2 ? argv[2] : DH_PRIMITIVE_VECTORS;

    if (!load_vectors(primitives) || !load_vectors(frames)) return 1;
    if (!load_session_material()) {
        printf("FAIL the published session material would not load\n");
        return 1;
    }

    test_the_hello_matches_the_golden_frame();
    test_negotiation_comes_from_the_reply();
    test_the_beat_only_fills_an_idle_direction();
    test_the_board_is_absent_only_after_the_window();
    test_an_unpaired_helper_is_told_so_and_waits();
    test_a_refused_open_is_an_unusable_device();
    test_a_partial_acquisition_that_completes_is_silent();
    test_a_cold_start_in_config_mode_says_config_mode();
    test_a_helper_that_never_sees_a_device_says_so();
    test_a_brief_disappearance_is_silent();
    test_the_backoff_caps_and_resets();
    test_a_flapping_link_is_reported_as_a_rate();
    test_pairing_round_trip();
    test_a_board_whose_key_changed_is_not_accepted();
    test_an_answer_to_someone_elses_question_is_dropped();
    test_the_listener_alert_expires_like_a_rate();
    test_a_bad_tag_drops_the_session_and_a_replay_does_not();
    test_a_session_end_is_acted_on();
    test_the_policy_predicates_are_decided_once();
    test_an_incompatible_board_refuses_bulk();
    test_an_unanswered_hello_is_a_dead_session();
    test_the_deadlines_survive_the_clock_wrapping();
    test_a_hello_that_cannot_be_built_is_still_reported();
    test_a_grant_nobody_asked_for_is_ignored();
    test_the_machine_owns_the_counter_bulk_goes_out_under();
    test_the_device_beat_is_traced_where_it_changes();
    test_the_beat_trace_does_not_outlive_its_session();
    test_the_beat_trace_starts_afresh_after_a_hello_is_refused();
    test_an_ack_for_someone_elses_hello_is_dropped();
    test_a_grant_answering_someone_elses_request_is_dropped();

    if (failures) {
        printf("%d helper check(s) failed\n", failures);
        return 1;
    }
    printf("helper tests passed\n");
    return 0;
}
