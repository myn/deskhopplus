/*
 * deskhopplus shared core — the session layer, protocol v2. See dh_session.h.
 */

#include "dh_session.h"

#include <string.h>

static uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd_u64(const uint8_t *p) {
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; i++) v |= (uint64_t)p[i] << (i * 8u);
    return v;
}

static void wr_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void wr_u32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (i * 8u));
}

static void wr_u64(uint8_t *p, uint64_t v) {
    for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (i * 8u));
}

/* ------------------------------------------------------------------ framing */

/*
 * An authenticated frame. The build moved to dh_auth_frame when the helper's
 * side needed the same thing (#80) — both ends build these, and "a header plus
 * a wrap" is not a rule worth having two implementations of.
 */
static dh_frame_result encode_tagged(uint8_t type, uint8_t flags,
                                     const uint8_t key[DH_SESSION_KEY_SIZE], uint64_t counter,
                                     const uint8_t *body, size_t body_len, uint8_t *out,
                                     size_t cap, size_t *out_len) {
    return dh_auth_frame(type, flags, key, counter, body, body_len, out, cap, out_len);
}

/* ------------------------------------------------------------------- codecs */

bool dh_hello_decode(const uint8_t *body, size_t len, dh_hello *out) {
    /* Fixed length. A hello carries no trailing anything in v2 — the token
       that used to be the remainder is what ADR-0008 removed — so a longer
       body is a frame this build does not understand rather than one it can
       read the front of. */
    if (body == NULL || out == NULL || len != DH_HELLO_LEN) return false;

    out->proto_version = rd_u16(body);
    out->os = body[2];
    out->build_type = body[3];
    out->channel_count = body[4];
    out->max_chunk = rd_u16(body + 5);
    out->correlation = rd_u64(body + 7);
    memcpy(out->helper_key_id, body + 15, DH_KEY_ID_SIZE);
    memcpy(out->helper_nonce, body + 23, DH_NONCE_SIZE);
    return true;
}

dh_frame_result dh_hello_encode(const dh_hello *in, const uint8_t key[DH_SESSION_KEY_SIZE],
                                uint64_t counter, uint8_t *out, size_t cap, size_t *out_len) {
    if (in == NULL) return DH_FRAME_ERR_BUFFER;

    uint8_t body[DH_HELLO_LEN];
    wr_u16(body, in->proto_version);
    body[2] = in->os;
    body[3] = in->build_type;
    body[4] = in->channel_count;
    wr_u16(body + 5, in->max_chunk);
    wr_u64(body + 7, in->correlation);
    memcpy(body + 15, in->helper_key_id, DH_KEY_ID_SIZE);
    memcpy(body + 23, in->helper_nonce, DH_NONCE_SIZE);

    return encode_tagged(DH_MSG_HELLO, 0, key, counter, body, sizeof body, out, cap, out_len);
}

bool dh_hello_ack_decode(const uint8_t *body, size_t len, dh_hello_ack *out) {
    if (body == NULL || out == NULL || len != DH_HELLO_ACK_LEN) return false;

    out->correlation = rd_u64(body);
    out->proto_version = rd_u16(body + 8);
    out->build_type = body[10];
    out->channel_count = body[11];
    out->max_chunk = rd_u16(body + 12);
    memcpy(out->board_nonce, body + 14, DH_NONCE_SIZE);
    return true;
}

dh_frame_result dh_hello_ack_encode(const dh_hello_ack *in, const uint8_t key[DH_SESSION_KEY_SIZE],
                                    uint64_t counter, uint8_t *out, size_t cap, size_t *out_len) {
    if (in == NULL) return DH_FRAME_ERR_BUFFER;

    uint8_t body[DH_HELLO_ACK_LEN];
    wr_u64(body, in->correlation);
    wr_u16(body + 8, in->proto_version);
    body[10] = in->build_type;
    body[11] = in->channel_count;
    wr_u16(body + 12, in->max_chunk);
    memcpy(body + 14, in->board_nonce, DH_NONCE_SIZE);

    return encode_tagged(DH_MSG_HELLO_ACK, 0, key, counter, body, sizeof body, out, cap, out_len);
}

bool dh_hello_refused_decode(const uint8_t *body, size_t len, dh_hello_refused *out) {
    if (body == NULL || out == NULL || len != DH_HELLO_REFUSED_LEN) return false;
    out->correlation = rd_u64(body);
    out->proto_version = rd_u16(body + 8);
    out->status = body[10];
    return true;
}

dh_frame_result dh_hello_refused_encode(const dh_hello_refused *in, uint8_t *out, size_t cap,
                                        size_t *out_len) {
    if (in == NULL) return DH_FRAME_ERR_BUFFER;

    uint8_t body[DH_HELLO_REFUSED_LEN];
    wr_u64(body, in->correlation);
    wr_u16(body + 8, in->proto_version);
    body[10] = in->status;

    return dh_frame_encode(DH_MSG_HELLO_REFUSED, 0, body, sizeof body, out, cap, out_len);
}

bool dh_pair_request_decode(const uint8_t *body, size_t len, dh_pair_request *out) {
    if (body == NULL || out == NULL || len != DH_PAIR_REQUEST_LEN) return false;
    out->correlation = rd_u64(body);
    memcpy(out->helper_public, body + 8, DH_P256_PUBLIC_SIZE);
    return true;
}

dh_frame_result dh_pair_request_encode(const dh_pair_request *in, uint8_t *out, size_t cap,
                                       size_t *out_len) {
    if (in == NULL) return DH_FRAME_ERR_BUFFER;

    uint8_t body[DH_PAIR_REQUEST_LEN];
    wr_u64(body, in->correlation);
    memcpy(body + 8, in->helper_public, DH_P256_PUBLIC_SIZE);

    return dh_frame_encode(DH_MSG_PAIR_REQUEST, 0, body, sizeof body, out, cap, out_len);
}

bool dh_pair_grant_decode(const uint8_t *body, size_t len, dh_pair_grant *out) {
    if (body == NULL || out == NULL || len != DH_PAIR_GRANT_LEN) return false;
    out->correlation = rd_u64(body);
    memcpy(out->board_public, body + 8, DH_P256_PUBLIC_SIZE);
    return true;
}

dh_frame_result dh_pair_grant_encode(const dh_pair_grant *in, uint8_t *out, size_t cap,
                                     size_t *out_len) {
    if (in == NULL) return DH_FRAME_ERR_BUFFER;

    uint8_t body[DH_PAIR_GRANT_LEN];
    wr_u64(body, in->correlation);
    memcpy(body + 8, in->board_public, DH_P256_PUBLIC_SIZE);

    return dh_frame_encode(DH_MSG_PAIR_GRANT, 0, body, sizeof body, out, cap, out_len);
}

bool dh_pair_refused_decode(const uint8_t *body, size_t len, dh_pair_refused *out) {
    if (body == NULL || out == NULL || len != DH_PAIR_REFUSED_LEN) return false;
    out->correlation = rd_u64(body);
    out->reason = body[8];
    return true;
}

dh_frame_result dh_pair_refused_encode(const dh_pair_refused *in, uint8_t *out, size_t cap,
                                       size_t *out_len) {
    if (in == NULL) return DH_FRAME_ERR_BUFFER;

    uint8_t body[DH_PAIR_REFUSED_LEN];
    wr_u64(body, in->correlation);
    body[8] = in->reason;

    return dh_frame_encode(DH_MSG_PAIR_REFUSED, 0, body, sizeof body, out, cap, out_len);
}

bool dh_listener_alert_decode(const uint8_t *body, size_t len, dh_listener_alert *out) {
    if (body == NULL || out == NULL || len != DH_LISTENER_ALERT_LEN) return false;
    out->window_ms = rd_u32(body);
    out->refused = rd_u32(body + 4);
    return true;
}

/* ------------------------------------------------------------------ session */

void dh_session_init(dh_session *s, uint8_t build_type) {
    memset(s, 0, sizeof *s);
    s->build_type = build_type;
}

void dh_session_stage_nonce(dh_session *s, const uint8_t nonce[DH_NONCE_SIZE]) {
    memcpy(s->staged_nonce, nonce, DH_NONCE_SIZE);
    s->nonce_staged = true;
}

void dh_session_drop(dh_session *s) {
    s->present = false;
    s->peer_os = 0;
    s->channel_count = 0;
    s->max_chunk = 0;
    s->last_seen_ms = 0;
    s->last_sent_ms = 0;

    /* The keys go with the session. A session that ended cannot have its
       counter space resumed — docs/protocol.md: "a retry is a new session, not
       a resumed one" — so leaving live keys behind would be the one way a
       counter could be reused under a key. */
    memset(s->k_h2b, 0, sizeof s->k_h2b);
    memset(s->k_b2h, 0, sizeof s->k_b2h);
    dh_auth_counter_init(&s->rx);
    s->tx_counter = 0;

    /* The listener count and the staged nonce deliberately survive: a client
       that keeps writing junk across a reconnection is exactly what the alert
       exists to notice, and entropy the caller already drew is not the
       session's to throw away. */
}

/* Anything that arrived and authenticated proves the helper is alive. Only a
   helper that already has a session — silence is what ends one, and a stray
   frame must not start one. */
static void note_received(dh_session *s, uint32_t now_ms) {
    if (s->present) s->last_seen_ms = now_ms;
}

void dh_session_note_sent(dh_session *s, uint32_t now_ms) {
    s->last_sent_ms = now_ms;
}

void dh_session_note_owed_sent(dh_session *s, uint8_t type) {
    /* The measurement is released only once something has taken the frame
       carrying it. See the header for why the beat needs no equivalent. */
    if (type == DH_MSG_LISTENER_ALERT) s->alert_pending = false;
}

/*
 * One frame that did not authenticate. Counted in a sliding window, separately
 * from frames that fail for other reasons, because a rate says what no single
 * event can (#94's shape).
 *
 * `as_registered` is the stronger signal: something named the registered
 * helper's key id and could not produce its tag. One of those closes the
 * window loudly on its own — the count exists to tell a probing listener from
 * a corrupt report, and this case is neither.
 *
 * Since #117 every *hello* that reaches here is `as_registered`, because one
 * naming any other key id is refused before the tag is checked. So the rate
 * threshold no longer does any work on the hello path — a single bad-tag hello
 * already trips the alert — and it earns its keep only on the in-session path
 * below, where bulk frames fail one at a time and a rate is the whole point.
 * That is a narrower net than before, deliberately; docs/protocol.md, *What an
 * unauthenticated frame causes*, says what is given up and why it is worth it.
 */
static void note_refused(dh_session *s, uint32_t now_ms, bool as_registered) {
    if (!s->window_started) {
        s->window_started = true;
        s->window_started_ms = now_ms;
        s->refused_in_window = 0;
        s->refused_as_registered = false;
    }
    if (s->refused_in_window < UINT32_MAX) s->refused_in_window++;
    if (as_registered) s->refused_as_registered = true;
}

/* A window that has run its length either becomes a pending alert or is
   forgotten. Runs whether or not a session exists: a board with no session has
   nobody to tell, so it keeps counting and reports on the next session. */
static void close_listener_window(dh_session *s, uint32_t now_ms) {
    if (!s->window_started) return;
    if ((uint32_t)(now_ms - s->window_started_ms) < DH_LISTENER_WINDOW_MS) return;

    if (s->refused_in_window >= DH_LISTENER_THRESHOLD || s->refused_as_registered) {
        s->alert.window_ms = DH_LISTENER_WINDOW_MS;
        s->alert.refused = s->refused_in_window;
        s->alert_pending = true;
    }

    s->window_started = false;
    s->refused_in_window = 0;
    s->refused_as_registered = false;
}

static uint16_t negotiate_chunk(uint16_t requested) {
    if (requested == 0 || requested < DH_SESSION_MIN_CHUNK) return DH_SESSION_MIN_CHUNK;
    return requested < DH_SESSION_MAX_CHUNK ? requested : DH_SESSION_MAX_CHUNK;
}

static uint8_t negotiate_channels(uint8_t requested) {
    if (requested == 0) return 1;
    return requested < DH_SESSION_CHANNEL_COUNT ? requested : DH_SESSION_CHANNEL_COUNT;
}

/*
 * A development build compiles the board's checks out entirely (#44). A
 * well-known development secret was rejected as worse than none: it has the
 * appearance of security and would eventually ship. The zeros below are not
 * pretending to be a secret — such a build says so in its build type, in its
 * product string and in the configuration UI, and the helper↔helper seal is
 * not a board's to disable in any build.
 */
static const uint8_t no_secret[DH_P256_SHARED_SIZE] = {0};

static bool skips_authentication(const dh_session *s) {
    return s->build_type == DH_BUILD_DEVELOPMENT;
}

static dh_frame_result refuse_hello(uint64_t correlation, uint8_t status, uint8_t *out, size_t cap,
                                    size_t *out_len) {
    const dh_hello_refused refused = {
        .correlation = correlation,
        .proto_version = DH_PROTO_VERSION,
        .status = status,
    };
    return dh_hello_refused_encode(&refused, out, cap, out_len);
}

/*
 * The board decides in this order and stops at the first that applies. The
 * order is docs/protocol.md's, not an implementation choice — two of these
 * conditions can hold at once, and which answer is given tells the user which
 * remedy to reach for.
 */
static dh_frame_result answer_hello(dh_session *s, dh_pair *pair, const dh_frame_view *f,
                                    uint32_t now_ms, uint8_t *out, size_t out_cap,
                                    size_t *out_len) {
    /*
     * Three fields are read before anything is verified: proto_version, so a
     * version this board does not implement is refused rather than failed on a
     * tag it could not have computed the same way; helper_key_id, so the board
     * knows whether the sender *claims* to be its registered helper; and
     * helper_nonce, because k_hello is derived from it — that read is what
     * makes the tag checkable at all. Nothing read here is recorded, acted on
     * or believed until the tag verifies.
     */
    /*
     * Zeroed rather than left indeterminate. `readable` being true is what
     * says this was filled, and a third compiler could not follow that across
     * the short circuit — MSVC at /W4 called it a potentially uninitialised
     * read (C4701) where gcc and clang had both been quiet.
     *
     * It is a false positive today, and the zeroing stays anyway: it costs
     * eight stores on a path that is about to do an HMAC, and it means a
     * future edit that reads this before the decode gets zeros rather than
     * whatever the stack held.
     */
    dh_hello hello = {0};
    const bool readable =
        f->hdr.len >= DH_FRAME_AUTH_PREFIX_SIZE &&
        dh_hello_decode(f->payload + DH_FRAME_AUTH_PREFIX_SIZE,
                        (size_t)f->hdr.len - DH_FRAME_AUTH_PREFIX_SIZE, &hello);

    /*
     * 1. A version this board does not implement. First, because a board
     *    cannot verify a tag under rules it does not know — and **a payload
     *    that is not a v2 hello's size is one of those versions**, decided
     *    here rather than by a length check that returns silence.
     *
     *    That distinction is not hypothetical. A v1 hello is 23 payload bytes
     *    and a v2 one is 63, so a length gate ahead of this would answer
     *    nothing at all to the one client certain to send the wrong shape: a
     *    helper this build predates. It would hear silence, which v2 reserves
     *    for a *failed tag*, and learn nothing about why.
     *
     *    Correlation 0, because a frame this build cannot parse has no
     *    correlation value to echo — and a helper acts only on an answer
     *    carrying its own, so an answer carrying nobody's is safe to overhear.
     */
    if (!readable)
        return refuse_hello(0, DH_HELLO_REFUSED_VERSION_INCOMPATIBLE, out, out_cap, out_len);

    if (hello.proto_version != DH_PROTO_VERSION)
        return refuse_hello(hello.correlation, DH_HELLO_REFUSED_VERSION_INCOMPATIBLE, out, out_cap,
                            out_len);

    /*
     * 2. No registration **for the key id this hello names**. There is no
     *    secret to prove and the honest remedy really is the config chord.
     *
     *    Per key id, not per board (#117). Asking only "does this board hold
     *    any registration?" left the case that matters — a board registered to
     *    *someone else* — falling through to the tag check, where it failed and
     *    drew silence. Silence takes a helper round the reconnect loop, never
     *    to `notPaired`, which is the one state that sends a PAIR_REQUEST; so
     *    the chord had nothing to provision and ADR-0008's "recovery is one
     *    chord press" did not hold. Ordinary ways in, no attacker: a helper
     *    identity regenerated (#112), a home directory restored onto a second
     *    Mac, the board plugged into a different computer.
     *
     *    Still safe to overhear, and for the same reason as before — the board
     *    cannot be provoked into saying anything false. It now says "I do not
     *    know *this* key" rather than "I know no key", which is a narrower true
     *    statement about its own registration and nothing about any helper. The
     *    refusal carries the asker's own correlation value, so the real helper
     *    discards a listener's, and #108's trap stays closed.
     *
     *    The NULL test is redundant — dh_pair_is_registered_key is false on an
     *    unregistered board too — and kept because this is the gate that
     *    decides whether there is a secret to derive under. It should read that
     *    way here, not one file away.
     */
    const uint8_t *shared_secret = dh_pair_shared_secret(pair);
    const bool known_helper =
        shared_secret != NULL && dh_pair_is_registered_key(pair, hello.helper_key_id);
    if (!known_helper && !skips_authentication(s))
        return refuse_hello(hello.correlation, DH_HELLO_REFUSED_UNPAIRED, out, out_cap, out_len);
    if (shared_secret == NULL) shared_secret = no_secret;

    /* 3. The tag. A failure draws nothing at all: answering is acting, and the
          answer would reach every attached client. That silence is what closes
          the manufactured chord trap (#108) — the sequence has no first step. */
    if (!skips_authentication(s)) {
        uint8_t k_hello[DH_SESSION_KEY_SIZE];
        dh_auth_derive_hello_key(shared_secret, hello.helper_nonce, k_hello);

        /* A fresh counter space per hello, so counter 0 is always usable: a
           retried hello carries a fresh nonce, which makes a fresh key, which
           makes a fresh space. There is no resumption. */
        dh_auth_counter counter;
        dh_auth_counter_init(&counter);

        const uint8_t *body = NULL;
        size_t body_len = 0;
        if (dh_auth_open(k_hello, &f->hdr, f->payload, &counter, &body, &body_len) != DH_AUTH_OK) {
            /* Always the stronger signal now: step 2 refused every hello that
               names a key id this board did not register, so anything reaching
               here claimed to be the registered helper and could not produce
               its tag. */
            note_refused(s, now_ms, true);
            return DH_FRAME_OK;
        }
    }

    /*
     * A board with no staged nonce cannot key a session, so it says nothing
     * and the helper retries. The caller stages one at boot and after each
     * hello, so this is a should-never-happen rather than a state to report.
     */
    if (!s->nonce_staged) return DH_FRAME_OK;

    dh_hello_ack ack = {
        .correlation = hello.correlation,
        .proto_version = DH_PROTO_VERSION,
        .build_type = s->build_type,
        .channel_count = negotiate_channels(hello.channel_count),
        .max_chunk = negotiate_chunk(hello.max_chunk),
    };
    memcpy(ack.board_nonce, s->staged_nonce, DH_NONCE_SIZE);

    dh_auth_derive_session_keys(shared_secret, hello.helper_nonce, s->staged_nonce, s->k_h2b,
                                s->k_b2h);
    dh_auth_counter_init(&s->rx);
    s->tx_counter = 0;

    s->present = true;
    s->peer_os = hello.os;
    s->channel_count = ack.channel_count;
    s->max_chunk = ack.max_chunk;
    s->last_seen_ms = now_ms;
    /* The ack about to be returned is this direction's first traffic, so the
       session starts with a full idle interval ahead of it rather than owing a
       beat immediately. */
    s->last_sent_ms = now_ms;

    const dh_frame_result rc =
        dh_hello_ack_encode(&ack, s->k_b2h, s->tx_counter, out, out_cap, out_len);
    if (rc == DH_FRAME_OK) {
        s->tx_counter++;
        s->nonce_staged = false; /* spent; the caller owes a fresh one */
    }
    return rc;
}

static dh_frame_result answer_pair_request(dh_session *s, dh_pair *pair, const dh_frame_view *f,
                                           uint32_t now_ms, uint8_t *out, size_t out_cap,
                                           size_t *out_len) {
    (void)s;

    dh_pair_request request;
    if (!dh_pair_request_decode(f->payload, f->hdr.len, &request))
        return DH_FRAME_OK; /* nothing to echo, so nothing to say */

    /* The board's only asymmetric work, and the reason this whole layer runs
       off the USB callback path: 133.4 ms, measured (#110). */
    const dh_pair_result rc = dh_pair_register(pair, now_ms, request.helper_public);

    if (rc == DH_PAIR_OK) {
        dh_pair_grant grant = {.correlation = request.correlation};
        memcpy(grant.board_public, dh_pair_public_key(pair), DH_P256_PUBLIC_SIZE);
        return dh_pair_grant_encode(&grant, out, out_cap, out_len);
    }

    /*
     * A key that is not a point on the curve, or a board with no identity of
     * its own, is answered with nothing. docs/protocol.md defines two refusal
     * reasons and neither of them is true here, and inventing a third to
     * describe a request no real helper sends would put a message on the wire
     * for the sole benefit of whatever sent the bad key. The window stays
     * open, so a garbage request cannot burn the user's minute.
     */
    if (rc == DH_PAIR_ERR_BAD_KEY || rc == DH_PAIR_ERR_NO_IDENTITY) return DH_FRAME_OK;

    const dh_pair_refused refused = {
        .correlation = request.correlation,
        .reason = (rc == DH_PAIR_ERR_ALREADY_REGISTERED) ? DH_PAIR_REFUSED_ALREADY_REGISTERED
                                                         : DH_PAIR_REFUSED_NO_WINDOW,
    };
    return dh_pair_refused_encode(&refused, out, out_cap, out_len);
}

dh_frame_result dh_session_on_frame(dh_session *s, dh_pair *pair, const dh_frame_view *f,
                                    uint32_t now_ms, uint8_t *out, size_t out_cap,
                                    size_t *out_len) {
    *out_len = 0;

    switch (f->hdr.type) {
        case DH_MSG_HELLO:
            return answer_hello(s, pair, f, now_ms, out, out_cap, out_len);

        case DH_MSG_PAIR_REQUEST:
            /*
             * Untagged by design — there is no key yet — so it proves nothing
             * about who sent it and is deliberately *not* liveness. Only a
             * window a physical chord opened makes it act on anything, and the
             * window is single-shot.
             */
            return answer_pair_request(s, pair, f, now_ms, out, out_cap, out_len);

        case DH_MSG_HEARTBEAT:
        case DH_MSG_POS_RESPONSE: {
            /*
             * The two authenticated types a helper sends that this layer does
             * not answer. They still have to authenticate before they count as
             * liveness: under v1 the deadline measured "something is writing",
             * and on a shared endpoint that is not the same claim (#95).
             *
             * The beat carries no more weight than any other frame — it exists
             * to fill a direction with nothing else in it, so that silence
             * means something.
             */
            const uint8_t *body = NULL;
            size_t body_len = 0;
            (void)dh_session_authenticate(s, f, now_ms, &body, &body_len);
            return DH_FRAME_OK;
        }

        default:
            /*
             * Everything else is either another layer's frame or a board→helper
             * type that arrived from the wrong direction — a listener writing
             * into the shared endpoint can send any of them. Ignored, and
             * deliberately not liveness: the rule that any frame proves the
             * helper alive rests on the frame being one this layer can act on.
             */
            return DH_FRAME_OK;
    }
}

dh_auth_result dh_session_authenticate(dh_session *s, const dh_frame_view *f, uint32_t now_ms,
                                       const uint8_t **body, size_t *body_len) {
    /*
     * No session, no keys — so there is nothing to relay under, and nothing to
     * check the frame against either.
     *
     * Deliberately **not** counted towards the listener alert, which counts
     * frames that fail *authentication* and not frames that fail for other
     * reasons. The difference is not pedantry: a session ends on three seconds
     * of silence and the helper does not learn of it instantly, so a helper
     * beating into a session the board has already evicted would otherwise
     * trip the alert on an ordinary liveness timeout — and #107 measured 586
     * of those in sixteen hours. An alarm that fires on the most common event
     * on this channel is an alarm a user learns to ignore.
     */
    if (!s->present) return DH_AUTH_ERR_NO_KEY;

    /* Malformed, not unauthenticated: too short to carry the prefix the tag
       lives in, so there is no tag that failed. */
    if (f->hdr.len < DH_FRAME_AUTH_PREFIX_SIZE) return DH_AUTH_ERR_SHORT;

    if (skips_authentication(s)) {
        *body = f->payload + DH_FRAME_AUTH_PREFIX_SIZE;
        *body_len = (size_t)f->hdr.len - DH_FRAME_AUTH_PREFIX_SIZE;
        note_received(s, now_ms);
        return DH_AUTH_OK;
    }

    /* A bad tag is a frame the holder of the registered key did not write. A
       bad counter is a frame it wrote once and something re-sent — the wire is
       ordered and does not duplicate, so a replay is the only way to see one.
       Both are counted. */
    const dh_auth_result rc = dh_auth_open(s->k_h2b, &f->hdr, f->payload, &s->rx, body, body_len);
    if (rc != DH_AUTH_OK) {
        note_refused(s, now_ms, false);
        return rc;
    }

    note_received(s, now_ms);
    return DH_AUTH_OK;
}

dh_frame_result dh_session_emit_relayed(dh_session *s, const dh_frame_view *f, uint8_t *out,
                                        size_t out_cap, size_t *out_len) {
    *out_len = 0;
    if (!s->present) return DH_FRAME_ERR_UNKNOWN_TYPE;

    const dh_frame_result rc = encode_tagged(f->hdr.type, f->hdr.flags, s->k_b2h, s->tx_counter,
                                             f->payload, f->hdr.len, out, out_cap, out_len);
    if (rc == DH_FRAME_OK) s->tx_counter++;
    return rc;
}

dh_frame_result dh_session_end(dh_session *s, uint8_t reason, uint8_t *out, size_t out_cap,
                               size_t *out_len) {
    *out_len = 0;
    if (!s->present) return DH_FRAME_OK;

    const uint8_t body[DH_SESSION_END_LEN] = {reason};
    const dh_frame_result rc = encode_tagged(DH_MSG_SESSION_END, 0, s->k_b2h, s->tx_counter, body,
                                             sizeof body, out, out_cap, out_len);

    /* Dropped after the frame is built, never before: dh_session_drop clears
       the very key this announcement is tagged under. */
    dh_session_drop(s);
    return rc;
}

dh_frame_result dh_session_tick(dh_session *s, uint32_t now_ms, uint8_t *out, size_t out_cap,
                                size_t *out_len) {
    *out_len = 0;

    /* Ahead of the session check: a board with no session still counts, and
       the window it measured is what the alert carries when one arrives. */
    close_listener_window(s, now_ms);

    if (!s->present) return DH_FRAME_OK;

    /* Unsigned difference throughout, so a wrapping millisecond counter is
       just arithmetic rather than a session dropped once every 49 days. */
    if ((uint32_t)(now_ms - s->last_seen_ms) >= (uint32_t)DH_SESSION_ABSENT_MS)
        return dh_session_end(s, DH_SESSION_END_LIVENESS_TIMEOUT, out, out_cap, out_len);

    /* A measurement waiting for someone to tell. Ahead of the beat because it
       is itself traffic — it fills the idle direction the beat would have — and
       because a rate that has already been measured should not wait another
       interval to be reported. */
    if (s->alert_pending) {
        uint8_t body[DH_LISTENER_ALERT_LEN];
        wr_u32(body, s->alert.window_ms);
        wr_u32(body + 4, s->alert.refused);

        const dh_frame_result rc = encode_tagged(DH_MSG_LISTENER_ALERT, 0, s->k_b2h, s->tx_counter,
                                                 body, sizeof body, out, out_cap, out_len);
        /*
         * The counter advances and the idle timer is charged, but the alert
         * stays pending until dh_session_note_owed_sent says the frame reached
         * the queue. Nothing follows an alert — a refused copy would be a
         * measurement destroyed, and the window it was taken over cannot be
         * measured a second time. The counter advancing on a frame that was
         * never sent is fine: a receiver refuses anything not strictly
         * greater, so gaps are ordinary.
         */
        if (rc == DH_FRAME_OK) {
            s->tx_counter++;
            dh_session_note_sent(s, now_ms);
        }
        return rc;
    }

    /* Fill an idle direction, and only an idle one. Anything else this board
       sent has already told the helper the same thing. */
    if ((uint32_t)(now_ms - s->last_sent_ms) < DH_SESSION_HEARTBEAT_MS) return DH_FRAME_OK;

    const dh_frame_result rc = encode_tagged(DH_MSG_DEVICE_HEARTBEAT, 0, s->k_b2h, s->tx_counter,
                                             NULL, 0, out, out_cap, out_len);

    /*
     * The timer is charged for a beat this layer *produced*, which is not
     * quite the same as one the transport managed to send — deliberately.
     * The caller's slot refuses only while it is occupied by a frame already
     * draining to this same helper, and that frame refreshes the helper just
     * as well as a beat would. A slot stuck for longer than that means the
     * endpoint is wedged, and a helper that genuinely cannot hear this board
     * is one that should be reconnecting, not one to keep reassuring.
     *
     * Retrying instead would encode a beat on every tick for as long as the
     * slot stayed busy, and each refusal would be counted as loss against
     * #69's diagnostic — turning a healthy transfer into an alarm.
     */
    if (rc == DH_FRAME_OK) {
        s->tx_counter++;
        dh_session_note_sent(s, now_ms);
    }
    return rc;
}
