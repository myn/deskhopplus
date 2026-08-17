/*
 * Session-layer tests for the shared core (#45): the hello / hello_ack
 * codecs and the device's liveness state.
 *
 * The golden vectors are the gate here too — the hello vectors are the
 * negotiated fields' definition, and the device's answer to hello_mac must
 * be hello_ack_ok byte for byte.
 *
 * Style follows frame_test.c: an assertion macro, a main, a printed failure
 * line, a non-zero exit — no framework.
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "dh_session.h"

/* The token in the hello_mac vector, which the device therefore holds as its
 * secret: the golden exchange only round-trips if authentication passes. */
static const uint8_t test_secret[DH_PAIR_SECRET_LEN] = {
    0xef, 0xbe, 0xad, 0xde, 0xef, 0xbe, 0xad, 0xde,
    0xef, 0xbe, 0xad, 0xde, 0xef, 0xbe, 0xad, 0xde,
};

static dh_pair test_pair;

static void reset_pairing(void) {
    dh_pair_init(&test_pair, test_secret);
}

static int failures = 0;

#define CHECK(cond, name, what)                                                 \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++failures;                                                         \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, (name), (what)); \
        }                                                                       \
    } while (0)

#define MAX_VECTOR_BYTES DH_FRAME_MAX_SIZE

static int hex_nibble(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Load one named vector from test-vectors/frames.txt. Returns its length,
 * or 0 when the name is absent — which is itself a failure at every call
 * site, since the vectors define what this layer must produce. */
static size_t load_vector(const char *path, const char *want, uint8_t *out, size_t cap) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[16384];
    size_t len = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '\n' || *p == '#') continue;
        char *bar = strchr(p, '|');
        if (!bar) continue;
        char name[64];
        size_t name_len = 0;
        for (char *q = p; q < bar && name_len + 1 < sizeof name; q++)
            if (!isspace((unsigned char)*q)) name[name_len++] = *q;
        name[name_len] = '\0';
        if (strcmp(name, want) != 0) continue;

        int hi = -1;
        for (char *q = bar + 1; *q; q++) {
            if (isspace((unsigned char)*q)) continue;
            int nib = hex_nibble((unsigned char)*q);
            if (nib < 0) break;
            if (hi < 0) {
                hi = nib;
            } else {
                if (len >= cap) break;
                out[len++] = (uint8_t)((hi << 4) | nib);
                hi = -1;
            }
        }
        break;
    }
    fclose(f);
    return len;
}

/* Decode a vector's frame and hand back its payload view. */
static bool vector_payload(const char *path, const char *name, uint8_t *raw, size_t raw_cap,
                           dh_frame_view *out) {
    size_t len = load_vector(path, name, raw, raw_cap);
    if (len == 0) return false;
    size_t consumed = 0;
    return dh_frame_decode(raw, len, out, &consumed) == DH_FRAME_OK && consumed == len;
}

/* The hello a helper on this platform would send, as a complete frame. */
static size_t encode_hello(uint8_t os, uint16_t version, uint8_t channels, uint16_t max_chunk,
                           const uint8_t *token, uint16_t token_len, uint8_t *out, size_t cap) {
    dh_hello h = {
        .proto_version = version,
        .os = os,
        .build_type = DH_BUILD_RELEASE,
        .channel_count = channels,
        .max_chunk = max_chunk,
        .token = token,
        .token_len = token_len,
    };
    size_t len = 0;
    if (dh_hello_encode(&h, out, cap, &len) != DH_FRAME_OK) return 0;
    return len;
}

/* Feed a complete frame to the session and return the reply length. */
static size_t feed(dh_session *s, const uint8_t *frame, size_t frame_len, uint32_t now_ms,
                   uint8_t *reply, size_t reply_cap) {
    dh_frame_view v;
    size_t consumed = 0;
    if (dh_frame_decode(frame, frame_len, &v, &consumed) != DH_FRAME_OK) return 0;
    size_t out_len = 0;
    if (dh_session_on_frame(s, &test_pair, &v, now_ms, reply, reply_cap, &out_len) != DH_FRAME_OK)
        return 0;
    return out_len;
}

/* Advance the clock, handing back whatever the device owed its helper. The
 * eviction transition *is* the SESSION_END frame — there is no separate
 * signal to disagree with it — so a tick that emits one is a tick that
 * dropped the session. */
static size_t tick(dh_session *s, uint32_t now_ms, uint8_t *out, size_t out_cap) {
    size_t out_len = 0;
    if (dh_session_tick(s, now_ms, out, out_cap, &out_len) != DH_FRAME_OK) return 0;
    return out_len;
}

/* Is this the frame the device was supposed to emit? */
static bool is_frame(const uint8_t *buf, size_t len, uint8_t type) {
    dh_frame_view v;
    size_t consumed = 0;
    return len > 0 && dh_frame_decode(buf, len, &v, &consumed) == DH_FRAME_OK && consumed == len &&
           v.hdr.type == type;
}

static bool is_session_end(const uint8_t *buf, size_t len, uint8_t reason) {
    dh_frame_view v;
    size_t consumed = 0;
    return len > 0 && dh_frame_decode(buf, len, &v, &consumed) == DH_FRAME_OK &&
           v.hdr.type == DH_MSG_SESSION_END && v.hdr.len == 1 && v.payload[0] == reason;
}

static void test_hello_codec_matches_vectors(const char *path) {
    uint8_t raw[MAX_VECTOR_BYTES];
    dh_frame_view v;

    CHECK(vector_payload(path, "hello_mac", raw, sizeof raw, &v), "hello_mac", "vector missing");
    dh_hello h;
    CHECK(dh_hello_decode(v.payload, v.hdr.len, &h), "hello_mac", "decode failed");
    CHECK(h.proto_version == DH_PROTO_VERSION, "hello_mac", "wrong protocol version");
    CHECK(h.os == DH_OS_MAC, "hello_mac", "wrong platform");
    CHECK(h.build_type == DH_BUILD_RELEASE, "hello_mac", "wrong build type");
    CHECK(h.channel_count == 1, "hello_mac", "wrong requested channel count");
    CHECK(h.max_chunk == 1024, "hello_mac", "wrong requested max chunk");
    CHECK(h.token_len == 16, "hello_mac", "wrong token length");

    /* Re-encode byte-identically: the codec is the wire format's definition. */
    uint8_t enc[MAX_VECTOR_BYTES];
    size_t enc_len = 0;
    CHECK(dh_hello_encode(&h, enc, sizeof enc, &enc_len) == DH_FRAME_OK, "hello_mac",
          "re-encode failed");
    size_t raw_len = load_vector(path, "hello_mac", raw, sizeof raw);
    CHECK(enc_len == raw_len && memcmp(enc, raw, raw_len) == 0, "hello_mac",
          "re-encode mismatch");

    struct {
        const char *name;
        uint8_t status;
        uint8_t channels;
        uint16_t max_chunk;
    } acks[] = {
        {"hello_ack_ok", DH_HELLO_OK, 1, 1024},
        {"hello_ack_auth_failed", DH_HELLO_AUTH_FAILED, 0, 0},
        {"hello_ack_version_mismatch", DH_HELLO_VERSION_INCOMPATIBLE, 0, 0},
    };
    for (size_t i = 0; i < sizeof acks / sizeof acks[0]; i++) {
        CHECK(vector_payload(path, acks[i].name, raw, sizeof raw, &v), acks[i].name,
              "vector missing");
        dh_hello_ack a;
        CHECK(dh_hello_ack_decode(v.payload, v.hdr.len, &a), acks[i].name, "decode failed");
        CHECK(a.status == acks[i].status, acks[i].name, "wrong status");
        CHECK(a.channel_count == acks[i].channels, acks[i].name, "wrong channel count");
        CHECK(a.max_chunk == acks[i].max_chunk, acks[i].name, "wrong max chunk");

        CHECK(dh_hello_ack_encode(&a, enc, sizeof enc, &enc_len) == DH_FRAME_OK, acks[i].name,
              "re-encode failed");
        raw_len = load_vector(path, acks[i].name, raw, sizeof raw);
        CHECK(enc_len == raw_len && memcmp(enc, raw, raw_len) == 0, acks[i].name,
              "re-encode mismatch");
    }
}

static void test_malformed_payloads_rejected(void) {
    uint8_t buf[DH_HELLO_FIXED_LEN + DH_HELLO_TOKEN_MAX] = {0};
    dh_hello h;
    dh_hello_ack a;

    CHECK(!dh_hello_decode(buf, DH_HELLO_FIXED_LEN - 1, &h), "hello", "short payload accepted");
    CHECK(!dh_hello_decode(NULL, DH_HELLO_FIXED_LEN, &h), "hello", "null payload accepted");
    CHECK(!dh_hello_decode(buf, sizeof buf + 1, &h), "hello", "over-long token accepted");
    CHECK(!dh_hello_ack_decode(buf, DH_HELLO_ACK_LEN - 1, &a), "hello_ack",
          "short payload accepted");
    CHECK(!dh_hello_ack_decode(buf, DH_HELLO_ACK_LEN + 1, &a), "hello_ack",
          "trailing bytes accepted");

    /* A caller's buffer too small for the frame is refused, not truncated. */
    dh_hello ok = {.proto_version = DH_PROTO_VERSION, .os = DH_OS_MAC};
    uint8_t tiny[4];
    size_t len = 0;
    CHECK(dh_hello_encode(&ok, tiny, sizeof tiny, &len) == DH_FRAME_ERR_BUFFER, "hello",
          "encode into a short buffer not refused");
}

/* The device's answer to the golden hello is the golden ack, byte for byte. */
static void test_device_answers_the_golden_hello(const char *path) {
    uint8_t hello[MAX_VECTOR_BYTES];
    const size_t hello_len = load_vector(path, "hello_mac", hello, sizeof hello);
    CHECK(hello_len > 0, "session", "hello_mac vector missing");

    uint8_t want[MAX_VECTOR_BYTES];
    const size_t want_len = load_vector(path, "hello_ack_ok", want, sizeof want);
    CHECK(want_len > 0, "session", "hello_ack_ok vector missing");

    dh_session s;
    dh_session_init(&s, DH_BUILD_RELEASE);
    reset_pairing();
    CHECK(!s.present, "session", "helper present before any hello");

    uint8_t reply[MAX_VECTOR_BYTES];
    const size_t reply_len = feed(&s, hello, hello_len, 5000, reply, sizeof reply);
    CHECK(reply_len == want_len && memcmp(reply, want, want_len) == 0, "session",
          "answer to hello_mac is not hello_ack_ok");
    CHECK(s.present, "session", "helper not present after hello");
    CHECK(s.peer_os == DH_OS_MAC, "session", "platform not recorded");
    CHECK(s.channel_count == DH_SESSION_CHANNEL_COUNT, "session", "channel count not negotiated");
    CHECK(s.max_chunk == 1024, "session", "chunk size not negotiated");
}

static void test_negotiation_clamps_to_what_the_device_has(void) {
    uint8_t hello[MAX_VECTOR_BYTES];
    uint8_t reply[MAX_VECTOR_BYTES];
    dh_hello_ack a;
    dh_frame_view v;
    size_t consumed = 0;

    /* A helper asking for three channels and a 4 KiB chunk gets what ships. */
    dh_session s;
    dh_session_init(&s, DH_BUILD_RELEASE);
    reset_pairing();
    size_t len = encode_hello(DH_OS_MAC, DH_PROTO_VERSION, 3, 4096, test_secret, sizeof test_secret, hello, sizeof hello);
    size_t reply_len = feed(&s, hello, len, 1000, reply, sizeof reply);
    CHECK(reply_len > 0, "negotiate", "no answer to a hello");
    CHECK(dh_frame_decode(reply, reply_len, &v, &consumed) == DH_FRAME_OK, "negotiate",
          "reply is not a frame");
    CHECK(v.hdr.type == DH_MSG_HELLO_ACK, "negotiate", "reply is not a hello_ack");
    CHECK(dh_hello_ack_decode(v.payload, v.hdr.len, &a), "negotiate", "ack decode failed");
    CHECK(a.status == DH_HELLO_OK, "negotiate", "over-asking refused rather than clamped");
    CHECK(a.channel_count == DH_SESSION_CHANNEL_COUNT, "negotiate", "channel count not clamped");
    CHECK(a.max_chunk == DH_SESSION_MAX_CHUNK, "negotiate", "chunk size not clamped");

    /* Asking for less than the device offers is honoured, not raised. */
    dh_session_init(&s, DH_BUILD_RELEASE);
    reset_pairing();
    len = encode_hello(DH_OS_WINDOWS, DH_PROTO_VERSION, 1, 512, test_secret, sizeof test_secret, hello, sizeof hello);
    reply_len = feed(&s, hello, len, 1000, reply, sizeof reply);
    CHECK(dh_frame_decode(reply, reply_len, &v, &consumed) == DH_FRAME_OK, "negotiate",
          "reply is not a frame");
    CHECK(dh_hello_ack_decode(v.payload, v.hdr.len, &a), "negotiate", "ack decode failed");
    CHECK(a.max_chunk == 512, "negotiate", "a smaller request was not honoured");
    CHECK(s.peer_os == DH_OS_WINDOWS, "negotiate", "platform not recorded");

    /* A helper asking for nothing usable is not handed a zero-sized chunk. */
    dh_session_init(&s, DH_BUILD_RELEASE);
    reset_pairing();
    len = encode_hello(DH_OS_MAC, DH_PROTO_VERSION, 0, 0, test_secret, sizeof test_secret, hello, sizeof hello);
    reply_len = feed(&s, hello, len, 1000, reply, sizeof reply);
    CHECK(dh_frame_decode(reply, reply_len, &v, &consumed) == DH_FRAME_OK, "negotiate",
          "reply is not a frame");
    CHECK(dh_hello_ack_decode(v.payload, v.hdr.len, &a), "negotiate", "ack decode failed");
    CHECK(a.status == DH_HELLO_OK, "negotiate", "a zero request refused rather than floored");
    CHECK(a.channel_count == 1 && a.max_chunk == DH_SESSION_MIN_CHUNK, "negotiate",
          "zero request not floored to a usable session");
}

static void test_version_mismatch_is_distinct_and_has_no_session(void) {
    dh_session s;
    dh_session_init(&s, DH_BUILD_RELEASE);
    reset_pairing();

    uint8_t hello[MAX_VECTOR_BYTES];
    uint8_t reply[MAX_VECTOR_BYTES];
    const size_t len =
        encode_hello(DH_OS_MAC, DH_PROTO_VERSION + 1, 1, 1024, test_secret, sizeof test_secret, hello, sizeof hello);
    const size_t reply_len = feed(&s, hello, len, 1000, reply, sizeof reply);

    dh_frame_view v;
    size_t consumed = 0;
    CHECK(dh_frame_decode(reply, reply_len, &v, &consumed) == DH_FRAME_OK, "version",
          "reply is not a frame");
    dh_hello_ack a;
    CHECK(dh_hello_ack_decode(v.payload, v.hdr.len, &a), "version", "ack decode failed");
    CHECK(a.status == DH_HELLO_VERSION_INCOMPATIBLE, "version", "mismatch not reported");
    CHECK(a.proto_version == DH_PROTO_VERSION, "version",
          "the device's own version is not in the ack");
    CHECK(a.channel_count == 0 && a.max_chunk == 0, "version",
          "effective fields not zeroed on a failed hello");
    CHECK(!s.present, "version", "an incompatible helper was admitted to a session");
}

/* A live session is not the mismatched hello's to destroy. Only one process
 * holds the channel, so a hello carrying a version the device already
 * negotiated past is anomalous — refuse it and leave the session standing,
 * rather than letting one stray frame end a working one. */
static void test_a_mismatched_hello_does_not_end_a_live_session(void) {
    dh_session s;
    dh_session_init(&s, DH_BUILD_RELEASE);
    reset_pairing();

    uint8_t hello[MAX_VECTOR_BYTES];
    uint8_t reply[MAX_VECTOR_BYTES];
    uint32_t now = 4000;
    size_t len = encode_hello(DH_OS_MAC, DH_PROTO_VERSION, 1, 1024, test_secret, sizeof test_secret, hello, sizeof hello);
    CHECK(feed(&s, hello, len, now, reply, sizeof reply) > 0, "version", "no answer to hello");
    CHECK(s.present, "version", "no session to begin with");

    now += 10;
    len = encode_hello(DH_OS_MAC, DH_PROTO_VERSION + 1, 1, 1024, test_secret, sizeof test_secret, hello, sizeof hello);
    const size_t reply_len = feed(&s, hello, len, now, reply, sizeof reply);

    dh_frame_view v;
    size_t consumed = 0;
    dh_hello_ack a;
    CHECK(dh_frame_decode(reply, reply_len, &v, &consumed) == DH_FRAME_OK, "version",
          "reply is not a frame");
    CHECK(dh_hello_ack_decode(v.payload, v.hdr.len, &a), "version", "ack decode failed");
    CHECK(a.status == DH_HELLO_VERSION_INCOMPATIBLE, "version", "mismatch not refused");
    CHECK(s.present, "version", "a stray mismatched hello ended a working session");
    /* Refused, but still a sign of life. This reverses the original rule
     * (ADR-0004): the channel is held exclusively, so the frame came from
     * the one process that owns this session, and a process that is writing
     * is alive — which is the only thing the deadline measures. Anomalous
     * and proves-nothing are different claims, and the anomaly is already
     * answered by refusing the hello without touching the session. */
    CHECK(s.last_seen_ms == now, "version", "a refused hello did not count as a sign of life");
}

/* A token pointer and its length must agree; the Swift binding builds this
 * struct by hand, so the invariant cannot rest on convention. */
static void test_a_token_length_without_a_token_is_refused(void) {
    dh_hello h = {
        .proto_version = DH_PROTO_VERSION,
        .os = DH_OS_MAC,
        .token = NULL,
        .token_len = 16,
    };
    uint8_t out[MAX_VECTOR_BYTES];
    size_t len = 0;
    CHECK(dh_hello_encode(&h, out, sizeof out, &len) != DH_FRAME_OK, "token",
          "a null token with a non-zero length was encoded");
}

static void test_the_ack_carries_the_device_build_type(void) {
    dh_session s;
    dh_session_init(&s, DH_BUILD_DEVELOPMENT);
    reset_pairing();

    uint8_t hello[MAX_VECTOR_BYTES];
    uint8_t reply[MAX_VECTOR_BYTES];
    const size_t len =
        encode_hello(DH_OS_MAC, DH_PROTO_VERSION, 1, 1024, test_secret, sizeof test_secret, hello, sizeof hello);
    const size_t reply_len = feed(&s, hello, len, 1000, reply, sizeof reply);

    dh_frame_view v;
    size_t consumed = 0;
    dh_hello_ack a;
    CHECK(dh_frame_decode(reply, reply_len, &v, &consumed) == DH_FRAME_OK, "build",
          "reply is not a frame");
    CHECK(dh_hello_ack_decode(v.payload, v.hdr.len, &a), "build", "ack decode failed");
    CHECK(a.build_type == DH_BUILD_DEVELOPMENT, "build",
          "a development build did not identify itself");
}

static void test_heartbeat_keeps_the_session_and_silence_ends_it(void) {
    dh_session s;
    dh_session_init(&s, DH_BUILD_RELEASE);
    reset_pairing();

    uint8_t hello[MAX_VECTOR_BYTES];
    uint8_t reply[MAX_VECTOR_BYTES];
    uint32_t now = 100000;
    const size_t len =
        encode_hello(DH_OS_MAC, DH_PROTO_VERSION, 1, 1024, test_secret, sizeof test_secret, hello, sizeof hello);
    CHECK(feed(&s, hello, len, now, reply, sizeof reply) > 0, "liveness", "no answer to hello");

    uint8_t beat[DH_FRAME_HEADER_SIZE];
    size_t beat_len = 0;
    CHECK(dh_frame_encode(DH_MSG_HEARTBEAT, 0, NULL, 0, beat, sizeof beat, &beat_len) ==
              DH_FRAME_OK,
          "liveness", "heartbeat encode failed");

    /* Beating on time keeps the helper present indefinitely, and a heartbeat
     * is not answered — it costs the device nothing but a timestamp. The
     * device's own beat is idle-gated, so a direction kept busy by note_sent
     * emits none of its own. */
    for (int i = 0; i < 10; i++) {
        now += DH_SESSION_HEARTBEAT_MS;
        dh_session_note_sent(&s, now);
        CHECK(tick(&s, now, reply, sizeof reply) == 0, "liveness",
              "marked absent, or beat into a direction that was not idle");
        CHECK(feed(&s, beat, beat_len, now, reply, sizeof reply) == 0, "liveness",
              "heartbeat drew a reply");
        CHECK(s.present, "liveness", "not present while beating");
    }

    /* One missed interval is not enough — a late beat is normal. The device
     * fills the now-idle direction with a beat of its own, which is not an
     * eviction. */
    now += DH_SESSION_HEARTBEAT_MS * DH_SESSION_MISSED_INTERVALS;
    CHECK(is_frame(reply, tick(&s, now, reply, sizeof reply), DH_MSG_DEVICE_HEARTBEAT), "liveness",
          "an idle direction did not draw the device's own beat");
    CHECK(s.present, "liveness", "absent after one missed interval");

    /* A couple of missed intervals is. The transition is the frame, and it
     * is emitted exactly once. */
    now += DH_SESSION_HEARTBEAT_MS;
    CHECK(is_session_end(reply, tick(&s, now, reply, sizeof reply),
                         DH_SESSION_END_LIVENESS_TIMEOUT),
          "liveness", "the timeout did not announce the eviction");
    CHECK(!s.present, "liveness", "still present after the timeout");
    now += DH_SESSION_HEARTBEAT_MS;
    CHECK(tick(&s, now, reply, sizeof reply) == 0, "liveness",
          "absence announced more than once, or a beat without a session");

    /* A heartbeat from a helper with no session does not resurrect one:
     * the session starts at hello, so the device knows what it negotiated. */
    CHECK(feed(&s, beat, beat_len, now, reply, sizeof reply) == 0, "liveness",
          "a stray heartbeat drew a reply");
    CHECK(!s.present, "liveness", "a stray heartbeat created a session");

    /* Re-hello after an absence starts a fresh session. */
    CHECK(feed(&s, hello, len, now, reply, sizeof reply) > 0, "liveness",
          "no answer to a re-hello");
    CHECK(s.present, "liveness", "re-hello did not restore the session");

    /* A dropped link does not wait for a timeout. */
    dh_session_drop(&s);
    CHECK(!s.present, "liveness", "a dropped link left the helper present");
}

static void test_liveness_survives_the_clock_wrapping(void) {
    dh_session s;
    dh_session_init(&s, DH_BUILD_RELEASE);
    reset_pairing();

    uint8_t hello[MAX_VECTOR_BYTES];
    uint8_t reply[MAX_VECTOR_BYTES];
    const size_t len =
        encode_hello(DH_OS_MAC, DH_PROTO_VERSION, 1, 1024, test_secret, sizeof test_secret, hello, sizeof hello);

    /* Hello just before the millisecond counter wraps; beat just after. Both
     * timers cross the wrap here — the liveness deadline and the idle timer
     * the device's own beat runs on. */
    const uint32_t before_wrap = UINT32_MAX - (DH_SESSION_HEARTBEAT_MS / 2);
    CHECK(feed(&s, hello, len, before_wrap, reply, sizeof reply) > 0, "wrap", "no answer to hello");

    const uint32_t after_wrap = before_wrap + DH_SESSION_HEARTBEAT_MS; /* wraps */
    CHECK(is_frame(reply, tick(&s, after_wrap, reply, sizeof reply), DH_MSG_DEVICE_HEARTBEAT),
          "wrap", "the idle timer did not survive the wrap");
    CHECK(s.present, "wrap", "marked absent across the wrap");

    CHECK(!is_session_end(reply, tick(&s, before_wrap + DH_SESSION_ABSENT_MS - 1, reply,
                                      sizeof reply),
                          DH_SESSION_END_LIVENESS_TIMEOUT),
          "wrap", "marked absent early across the wrap");

    CHECK(is_session_end(reply,
                         tick(&s, before_wrap + DH_SESSION_ABSENT_MS + 1, reply, sizeof reply),
                         DH_SESSION_END_LIVENESS_TIMEOUT),
          "wrap", "not marked absent across the wrap");
}

/*
 * The device's beat fills an idle direction and nothing else. It is not the
 * liveness signal — traffic is — so a direction carrying anything at all
 * emits none. This is what keeps a sustained transfer from starving the beat
 * out of the one outbound frame slot and manufacturing a false eviction
 * (ADR-0004).
 */
static void test_the_device_beats_only_into_an_idle_direction(void) {
    dh_session s;
    dh_session_init(&s, DH_BUILD_RELEASE);
    reset_pairing();

    uint8_t hello[MAX_VECTOR_BYTES];
    uint8_t reply[MAX_VECTOR_BYTES];
    uint32_t now = 500000;
    const size_t len = encode_hello(DH_OS_MAC, DH_PROTO_VERSION, 1, 1024, test_secret,
                                    sizeof test_secret, hello, sizeof hello);
    CHECK(feed(&s, hello, len, now, reply, sizeof reply) > 0, "idle", "no answer to hello");

    /* The ack is itself traffic, so the direction is not idle yet. */
    now += DH_SESSION_HEARTBEAT_MS - 1;
    CHECK(tick(&s, now, reply, sizeof reply) == 0, "idle", "beat before the direction was idle");

    /* A full interval of nothing draws one beat, and only one. */
    now += 1;
    CHECK(is_frame(reply, tick(&s, now, reply, sizeof reply), DH_MSG_DEVICE_HEARTBEAT), "idle",
          "an idle interval drew no beat");
    CHECK(tick(&s, now, reply, sizeof reply) == 0, "idle", "beat twice in one interval");

    /* Something else going out resets the idle timer just as a beat does:
     * whatever occupied the slot has already proved the device alive. */
    for (int i = 0; i < 5; i++) {
        now += DH_SESSION_HEARTBEAT_MS - 1;
        dh_session_note_sent(&s, now);
        CHECK(tick(&s, now, reply, sizeof reply) == 0, "idle",
              "a busy direction still drew a beat");
        /* Keep the helper's own liveness fresh so this is about idleness. */
        CHECK(feed(&s, hello, len, now, reply, sizeof reply) > 0, "idle", "no answer to re-hello");
    }

    /* No session, no beat — its absence is what makes the helper's timeout
     * mean something. */
    dh_session_drop(&s);
    now += DH_SESSION_HEARTBEAT_MS * 4;
    CHECK(tick(&s, now, reply, sizeof reply) == 0, "idle", "beat with no session to keep alive");
}

/*
 * Liveness is carried by traffic, not by the beat. A helper busy sending real
 * frames suppresses its own beat, so a device that credited only beats would
 * evict it in the middle of its own traffic — the mirror of the starvation
 * the device's beat is idle-gated to avoid (ADR-0004).
 */
static void test_any_frame_from_the_helper_is_liveness(void) {
    dh_session s;
    dh_session_init(&s, DH_BUILD_RELEASE);
    reset_pairing();

    uint8_t hello[MAX_VECTOR_BYTES];
    uint8_t reply[MAX_VECTOR_BYTES];
    uint32_t now = 900000;
    const size_t len = encode_hello(DH_OS_MAC, DH_PROTO_VERSION, 1, 1024, test_secret,
                                    sizeof test_secret, hello, sizeof hello);
    CHECK(feed(&s, hello, len, now, reply, sizeof reply) > 0, "traffic", "no answer to hello");

    /* A position response is not a beat, is not answered, and is not even
       this layer's frame — and it still proves the helper is there. */
    uint8_t pos[DH_FRAME_HEADER_SIZE + 5];
    size_t pos_len = 0;
    const uint8_t pos_payload[5] = {1, 0x00, 0x40, 0x00, 0xC0};
    CHECK(dh_frame_encode(DH_MSG_POS_RESPONSE, 0, pos_payload, sizeof pos_payload, pos, sizeof pos,
                          &pos_len) == DH_FRAME_OK,
          "traffic", "pos_response encode failed");

    for (int i = 0; i < 10; i++) {
        now += DH_SESSION_HEARTBEAT_MS * DH_SESSION_MISSED_INTERVALS;
        CHECK(feed(&s, pos, pos_len, now, reply, sizeof reply) == 0, "traffic",
              "a position response drew a reply");
        (void)tick(&s, now, reply, sizeof reply);
        CHECK(s.present, "traffic", "a helper sending real frames was evicted for not beating");
    }

    /* And silence still ends it — traffic refreshes the deadline, it does
       not remove it. */
    now += DH_SESSION_ABSENT_MS;
    CHECK(is_session_end(reply, tick(&s, now, reply, sizeof reply),
                         DH_SESSION_END_LIVENESS_TIMEOUT),
          "traffic", "silence after traffic did not end the session");
}

/*
 * An eviction the device knows about is announced rather than left to the
 * helper's timeout. It is an optimisation over that timeout and never a
 * substitute — a device that reboots announces nothing — so the only thing
 * under test is that it says so when it can.
 */
static void test_an_eviction_the_device_knows_about_is_announced(void) {
    dh_session s;
    dh_session_init(&s, DH_BUILD_RELEASE);
    reset_pairing();

    uint8_t hello[MAX_VECTOR_BYTES];
    uint8_t reply[MAX_VECTOR_BYTES];
    size_t out_len = 0;
    const uint32_t now = 700000;
    const size_t len = encode_hello(DH_OS_MAC, DH_PROTO_VERSION, 1, 1024, test_secret,
                                    sizeof test_secret, hello, sizeof hello);

    /* A framing error on the device's reader: the stream is untrustworthy
     * and the helper is told, rather than going on writing into a reader it
     * has desynchronised. */
    CHECK(feed(&s, hello, len, now, reply, sizeof reply) > 0, "end", "no answer to hello");
    CHECK(dh_session_end(&s, DH_SESSION_END_PROTOCOL_ERROR, reply, sizeof reply, &out_len) ==
              DH_FRAME_OK,
          "end", "ending on a protocol error failed");
    CHECK(is_session_end(reply, out_len, DH_SESSION_END_PROTOCOL_ERROR), "end",
          "a protocol error did not announce the end");
    CHECK(!s.present, "end", "a protocol error left the session up");

    /* Nothing to end, nothing to say. A helper that never had a session
     * would otherwise be told one of its had ended. */
    out_len = 0;
    CHECK(dh_session_end(&s, DH_SESSION_END_PROTOCOL_ERROR, reply, sizeof reply, &out_len) ==
              DH_FRAME_OK,
          "end", "ending a session that never started was an error");
    CHECK(out_len == 0, "end", "announced the end of a session that never started");

    /* The silent form is for a link that is already gone: there is nobody
     * left to tell, and the bytes would go nowhere. */
    CHECK(feed(&s, hello, len, now, reply, sizeof reply) > 0, "end", "no answer to re-hello");
    dh_session_drop(&s);
    CHECK(!s.present, "end", "a dropped link left the helper present");
}

/* The announcement is the wire format's, not this build's idea of it. */
static void test_session_end_matches_the_vectors(const char *path) {
    static const struct {
        const char *name;
        uint8_t reason;
    } cases[] = {
        {"session_end_liveness", DH_SESSION_END_LIVENESS_TIMEOUT},
        {"session_end_protocol_error", DH_SESSION_END_PROTOCOL_ERROR},
        {"session_end_unpaired", DH_SESSION_END_UNPAIRED},
    };

    uint8_t raw[MAX_VECTOR_BYTES];
    uint8_t reply[MAX_VECTOR_BYTES];

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const size_t want = load_vector(path, cases[i].name, raw, sizeof raw);
        CHECK(want > 0, cases[i].name, "vector missing");

        dh_session s;
        dh_session_init(&s, DH_BUILD_RELEASE);
        reset_pairing();

        uint8_t hello[MAX_VECTOR_BYTES];
        const size_t len = encode_hello(DH_OS_MAC, DH_PROTO_VERSION, 1, 1024, test_secret,
                                        sizeof test_secret, hello, sizeof hello);
        CHECK(feed(&s, hello, len, 1000, reply, sizeof reply) > 0, cases[i].name,
              "no answer to hello");

        size_t out_len = 0;
        CHECK(dh_session_end(&s, cases[i].reason, reply, sizeof reply, &out_len) == DH_FRAME_OK,
              cases[i].name, "encode failed");
        CHECK(out_len == want && memcmp(reply, raw, want) == 0, cases[i].name,
              "the device's announcement is not the golden frame");
    }

    /* The device's own beat, likewise. */
    const size_t want = load_vector(path, "device_heartbeat", raw, sizeof raw);
    CHECK(want > 0, "device_heartbeat", "vector missing");

    dh_session s;
    dh_session_init(&s, DH_BUILD_RELEASE);
    reset_pairing();
    uint8_t hello[MAX_VECTOR_BYTES];
    const size_t len = encode_hello(DH_OS_MAC, DH_PROTO_VERSION, 1, 1024, test_secret,
                                    sizeof test_secret, hello, sizeof hello);
    CHECK(feed(&s, hello, len, 1000, reply, sizeof reply) > 0, "device_heartbeat",
          "no answer to hello");
    const size_t beat_len = tick(&s, 1000 + DH_SESSION_HEARTBEAT_MS, reply, sizeof reply);
    CHECK(beat_len == want && memcmp(reply, raw, want) == 0, "device_heartbeat",
          "the device's beat is not the golden frame");
}

static void test_other_bands_are_not_this_layers_business(void) {
    dh_session s;
    dh_session_init(&s, DH_BUILD_RELEASE);
    reset_pairing();

    uint8_t hello[MAX_VECTOR_BYTES];
    uint8_t reply[MAX_VECTOR_BYTES];
    const uint32_t now = 7000;
    const size_t len =
        encode_hello(DH_OS_MAC, DH_PROTO_VERSION, 1, 1024, test_secret, sizeof test_secret, hello, sizeof hello);
    CHECK(feed(&s, hello, len, now, reply, sizeof reply) > 0, "bands", "no answer to hello");

    /* A bulk frame is relayed opaquely (#47) and never answered here — but
     * it is a sign of life, which reverses the original rule (ADR-0004). A
     * helper suppresses its own beat while it has real traffic to send, so a
     * device crediting only beats would evict it in the middle of a transfer.
     * In the firmware bulk never reaches this function at all; channel.c
     * notes it on the relay path, and this covers the rule itself. */
    uint8_t chunk[64];
    size_t chunk_len = 0;
    const uint8_t body[12] = {0};
    CHECK(dh_frame_encode(DH_MSG_CLIP_CHUNK, 0, body, sizeof body, chunk, sizeof chunk,
                          &chunk_len) == DH_FRAME_OK,
          "bands", "chunk encode failed");
    CHECK(feed(&s, chunk, chunk_len, now + 10, reply, sizeof reply) == 0, "bands",
          "a bulk frame drew a session reply");
    CHECK(s.last_seen_ms == now + 10, "bands", "a bulk frame did not count as a sign of life");

    /* Pairing is #46's; this layer stays silent rather than guessing. */
    uint8_t pair[DH_FRAME_HEADER_SIZE];
    size_t pair_len = 0;
    CHECK(dh_frame_encode(DH_MSG_PAIR_REQUEST, 0, NULL, 0, pair, sizeof pair, &pair_len) ==
              DH_FRAME_OK,
          "bands", "pair_request encode failed");
    CHECK(feed(&s, pair, pair_len, now + 20, reply, sizeof reply) == 0, "bands",
          "pair_request answered before #46 decides what to answer");
}

static void test_a_malformed_hello_is_not_a_session(void) {
    dh_session s;
    dh_session_init(&s, DH_BUILD_RELEASE);
    reset_pairing();

    /* A hello whose payload is too short to hold the negotiated fields. */
    uint8_t frame[16];
    size_t frame_len = 0;
    const uint8_t stub[3] = {1, 0, DH_OS_MAC};
    CHECK(dh_frame_encode(DH_MSG_HELLO, 0, stub, sizeof stub, frame, sizeof frame, &frame_len) ==
              DH_FRAME_OK,
          "malformed", "encode failed");

    uint8_t reply[MAX_VECTOR_BYTES];
    CHECK(feed(&s, frame, frame_len, 1000, reply, sizeof reply) == 0, "malformed",
          "a truncated hello drew a reply");
    CHECK(!s.present, "malformed", "a truncated hello created a session");
}

/* The gate everything else on the channel sits behind: an unpaired helper is
 * refused, told so distinctly, and relayed nothing for. */
static void test_an_unpaired_helper_is_refused_and_told_which_remedy(void) {
    dh_session s;
    dh_session_init(&s, DH_BUILD_RELEASE);
    dh_pair_init(&test_pair, NULL); /* a wiped device: no secret at all */

    uint8_t hello[MAX_VECTOR_BYTES];
    uint8_t reply[MAX_VECTOR_BYTES];
    const size_t len = encode_hello(DH_OS_MAC, DH_PROTO_VERSION, 1, 1024, test_secret,
                                    sizeof test_secret, hello, sizeof hello);
    const size_t reply_len = feed(&s, hello, len, 1000, reply, sizeof reply);

    dh_frame_view v;
    size_t consumed = 0;
    dh_hello_ack a;
    CHECK(dh_frame_decode(reply, reply_len, &v, &consumed) == DH_FRAME_OK, "auth",
          "reply is not a frame");
    CHECK(dh_hello_ack_decode(v.payload, v.hdr.len, &a), "auth", "ack decode failed");
    CHECK(a.status == DH_HELLO_AUTH_FAILED, "auth", "an unpaired helper was not refused");
    CHECK(a.status != DH_HELLO_VERSION_INCOMPATIBLE, "auth",
          "authentication failure was reported as a version mismatch");
    CHECK(a.channel_count == 0 && a.max_chunk == 0, "auth",
          "effective fields not zeroed on a refused hello");
    CHECK(!dh_session_may_relay(&s), "auth", "the device would relay for an unpaired peer");

    /* Outside a window, asking to pair gets nothing back. */
    uint8_t pair_req[DH_FRAME_HEADER_SIZE];
    size_t req_len = 0;
    (void)dh_frame_encode(DH_MSG_PAIR_REQUEST, 0, NULL, 0, pair_req, sizeof pair_req, &req_len);
    CHECK(feed(&s, pair_req, req_len, 2000, reply, sizeof reply) == 0, "auth",
          "a pairing request was granted outside a window");

    /* A chord press, and the same helper is provisioned with no interaction. */
    const uint8_t fresh[DH_PAIR_SECRET_LEN] = {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
    dh_pair_open_window(&test_pair, fresh, 3000);
    const size_t grant_len = feed(&s, pair_req, req_len, 3000, reply, sizeof reply);
    CHECK(dh_frame_decode(reply, grant_len, &v, &consumed) == DH_FRAME_OK, "auth",
          "the grant is not a frame");
    CHECK(v.hdr.type == DH_MSG_PAIR_GRANT, "auth", "the reply is not a pair grant");
    CHECK(v.hdr.len == DH_PAIR_SECRET_LEN, "auth", "the grant is not a secret");

    /* And the helper that stored it now gets a session. */
    uint8_t token[DH_PAIR_SECRET_LEN];
    memcpy(token, v.payload, sizeof token);
    const size_t paired_len = encode_hello(DH_OS_MAC, DH_PROTO_VERSION, 1, 1024, token,
                                           sizeof token, hello, sizeof hello);
    (void)feed(&s, hello, paired_len, 4000, reply, sizeof reply);
    CHECK(dh_session_may_relay(&s), "auth", "a freshly paired helper still cannot relay");

    reset_pairing();
}

/* Development builds compile the check out — a well-known development secret
 * was rejected as worse than none — and say so in the build type. */
static void test_a_development_build_needs_no_secret(void) {
    dh_session s;
    dh_session_init(&s, DH_BUILD_DEVELOPMENT);
    dh_pair_init(&test_pair, NULL);

    uint8_t hello[MAX_VECTOR_BYTES];
    uint8_t reply[MAX_VECTOR_BYTES];
    const size_t len =
        encode_hello(DH_OS_MAC, DH_PROTO_VERSION, 1, 1024, NULL, 0, hello, sizeof hello);
    const size_t reply_len = feed(&s, hello, len, 1000, reply, sizeof reply);

    dh_frame_view v;
    size_t consumed = 0;
    dh_hello_ack a;
    CHECK(dh_frame_decode(reply, reply_len, &v, &consumed) == DH_FRAME_OK, "dev",
          "reply is not a frame");
    CHECK(dh_hello_ack_decode(v.payload, v.hdr.len, &a), "dev", "ack decode failed");
    CHECK(a.status == DH_HELLO_OK, "dev", "a development build still demanded a secret");
    CHECK(a.build_type == DH_BUILD_DEVELOPMENT, "dev",
          "a development build did not identify itself");
    CHECK(dh_session_may_relay(&s), "dev", "a development build refused to relay");

    reset_pairing();
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : DH_TEST_VECTORS;
    reset_pairing();

    test_hello_codec_matches_vectors(path);
    test_malformed_payloads_rejected();
    test_device_answers_the_golden_hello(path);
    test_negotiation_clamps_to_what_the_device_has();
    test_version_mismatch_is_distinct_and_has_no_session();
    test_a_mismatched_hello_does_not_end_a_live_session();
    test_a_token_length_without_a_token_is_refused();
    test_the_ack_carries_the_device_build_type();
    test_heartbeat_keeps_the_session_and_silence_ends_it();
    test_liveness_survives_the_clock_wrapping();
    test_the_device_beats_only_into_an_idle_direction();
    test_any_frame_from_the_helper_is_liveness();
    test_an_eviction_the_device_knows_about_is_announced();
    test_session_end_matches_the_vectors(path);
    test_other_bands_are_not_this_layers_business();
    test_a_malformed_hello_is_not_a_session();
    test_an_unpaired_helper_is_refused_and_told_which_remedy();
    test_a_development_build_needs_no_secret();

    if (failures) {
        printf("%d session check(s) failed\n", failures);
        return 1;
    }
    printf("session tests passed\n");
    return 0;
}
