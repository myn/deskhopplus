/*
 * The helper's side of the session (#79, #80). See dh_helper.h for what lives
 * here and what deliberately does not.
 *
 * Lifted from SessionEngine.swift, which held the only copy. Every behaviour
 * below was measured or argued for on that side first; where a comment says
 * why, it is because the reason is not visible in the code and was paid for
 * once already.
 */

#include "dh_helper.h"

#include <string.h>

/* Every frame this machine emits has to fit one output slot. A slot sized for
   the wrong one would truncate rather than fail — the shape of the defect #109
   found on the board's reply buffer, where a v1-sized buffer meant a v2 board
   never answered a pairing request at all. */
_Static_assert(DH_FRAME_HEADER_SIZE + DH_FRAME_AUTH_PREFIX_SIZE + DH_HELLO_LEN <=
                   DH_HELPER_FRAME_MAX,
               "the hello must fit an output slot");
_Static_assert(DH_P256_PUBLIC_SIZE <= DH_HELPER_FRAME_MAX,
               "the board key handed over for storage must fit an output slot");

/* Unsigned differences throughout: a wrapping millisecond counter is then just
   arithmetic, not a session dropped once every 49 days. */
static bool elapsed(uint32_t now_ms, uint32_t since_ms, uint32_t span_ms) {
    return (uint32_t)(now_ms - since_ms) >= span_ms;
}

/* ------------------------------------------------------------------ outputs */

void dh_helper_outputs_reset(dh_helper_outputs *o) {
    o->count = 0;
    o->overflow = 0;
}

static dh_helper_output *slot(dh_helper_outputs *o) {
    if (o->count >= DH_HELPER_OUTPUTS_MAX) {
        o->overflow++;
        return NULL;
    }
    dh_helper_output *item = &o->items[o->count++];
    memset(item, 0, sizeof *item);
    return item;
}

static void put(dh_helper_outputs *o, dh_helper_output_kind kind) {
    dh_helper_output *item = slot(o);
    if (item != NULL) item->kind = (uint8_t)kind;
}

static void put_note(dh_helper_outputs *o, dh_helper_note note, int32_t a, int32_t b) {
    dh_helper_output *item = slot(o);
    if (item == NULL) return;
    item->kind = DH_HELPER_OUT_NOTE;
    item->note = (uint8_t)note;
    item->a = a;
    item->b = b;
}

static void put_bytes(dh_helper_outputs *o, dh_helper_output_kind kind, const uint8_t *bytes,
                      size_t len) {
    dh_helper_output *item = slot(o);
    if (item == NULL) return;
    item->kind = (uint8_t)kind;
    /* The slot is sized by DH_HELPER_FRAME_MAX, the largest frame this machine
       emits. A longer one is a bug in this file, not a truncation to hide. */
    if (len > sizeof item->bytes) len = sizeof item->bytes;
    memcpy(item->bytes, bytes, len);
    item->len = len;
}

static void put_retry(dh_helper_outputs *o, uint32_t delay_ms) {
    dh_helper_output *item = slot(o);
    if (item == NULL) return;
    item->kind = DH_HELPER_OUT_RETRY;
    item->a = (int32_t)delay_ms;
}

/* A state is reported only when it changes. Everything that decides one goes
   through here, so no caller has to remember. */
static bool set_state(dh_helper *h, dh_helper_outputs *o, dh_helper_state next) {
    if (h->state == next) return false;
    h->state = next;
    dh_helper_output *item = slot(o);
    if (item != NULL) {
        item->kind = DH_HELPER_OUT_STATE;
        item->state = (uint8_t)next;
    }
    return true;
}

/* ------------------------------------------------------------------ backoff */

static uint32_t backoff_next(dh_helper *h) {
    const uint32_t delay = h->backoff_ms;
    h->backoff_ms = delay * 2u > DH_HELPER_BACKOFF_CAP_MS ? DH_HELPER_BACKOFF_CAP_MS : delay * 2u;
    return delay;
}

static void backoff_reset(dh_helper *h) { h->backoff_ms = DH_HELPER_BACKOFF_FIRST_MS; }

/* ------------------------------------------------------------- session state */

static void forget_crypto_state(dh_helper *h) {
    memset(h->k_h2b, 0, sizeof h->k_h2b);
    memset(h->k_b2h, 0, sizeof h->k_b2h);
    memset(h->helper_nonce, 0, sizeof h->helper_nonce);
    h->have_keys = false;
    h->have_nonce = false;
    h->listener_alert_live = false;
    h->tx_counter = 0;
    dh_auth_counter_init(&h->rx);
}

static void forget_beat_trace(dh_helper *h) {
    h->have_device_beat = false;
    h->beat_quiet_noted = false;
}

static void forget_session(dh_helper *h) {
    h->phase = DH_HELPER_PHASE_IDLE;
    h->have_negotiated = false;
    /* The totals belong to the board this session was with. Every session is
       told a fresh baseline, so keeping the old ones would only mean a stall
       in the first moments of a new session quoting the last board's (#133). */
    h->have_device_drops = false;
    dh_frame_reader_init(&h->reader);
    forget_beat_trace(h);
    forget_crypto_state(h);
}

/* -------------------------------------------------------- the reconnect rate */

/*
 * Two rings, because one window cannot answer both faults. See the constants
 * in dh_helper.h for which is which and why widening the short one is not the
 * same change.
 */
static void push_time(uint32_t *ring, size_t *count, size_t cap, uint32_t now_ms) {
    if (*count == cap) {
        memmove(ring, ring + 1, (cap - 1) * sizeof ring[0]);
        (*count)--;
    }
    ring[(*count)++] = now_ms;
}

static bool rate_reached(const uint32_t *ring, size_t count, size_t need, uint32_t window_ms,
                         uint32_t now_ms) {
    if (count < need) return false;
    return (uint32_t)(now_ms - ring[0]) <= window_ms;
}

/* False when the drop was not counted, so a caller feeding the second ring
   makes the same decision about the same event rather than a second one. */
static bool record_drop(dh_helper *h, uint32_t now_ms) {
    if (!h->holding_channels && h->phase == DH_HELPER_PHASE_IDLE) return false;

    /*
     * The same disappearance, arriving again. One physical event raises one
     * notification per HID interface, so counting each of them measures how
     * many collections the device has rather than how well the link is
     * holding — and it spent three of the four drops on a config-mode return
     * (#126).
     */
    if (h->drop_count > 0 &&
        (uint32_t)(now_ms - h->recent_drops[h->drop_count - 1]) < DH_HELPER_DROP_DEBOUNCE_MS)
        return false;

    push_time(h->recent_drops, &h->drop_count, DH_HELPER_RECONNECT_LIMIT, now_ms);
    return true;
}

/*
 * Whether this teardown is far enough from the one before it to be outside
 * anything the short window could have measured — which is what the slow ring
 * is for. Two things about the comparison were each got wrong once:
 *
 * **Against the previous teardown, not the previous ring entry.** Comparing
 * against the entry lets a flap at just under the threshold land every second
 * one and fill the ring at half rate.
 *
 * **A whole short window, not the average spacing inside one.** Four inside
 * thirty seconds averages one every seven and a half, and admitting anything
 * slower than *that* admits an eight-second flap three times over — a burst
 * squarely inside what the short window reports, which then held the state
 * line for three quarters of an hour after the link had healed. Measured.
 * That is a latch rather than a reading, the mistake #94 corrected.
 *
 * The cost is a band the two readings share no cover for: teardowns spaced
 * from about ten seconds to thirty, too slow for four to fit the short window
 * and too fast to reach this one. Nothing has been observed there, and the
 * alternative is an alarm that stands for forty-five minutes every time a
 * cable is jiggled — which is the alarm a user learns to ignore.
 *
 * Read before this drop is recorded, so recent_drops still ends with the one
 * before it.
 */
static bool stands_alone(const dh_helper *h, uint32_t now_ms) {
    if (h->drop_count == 0) return true;
    return (uint32_t)(now_ms - h->recent_drops[h->drop_count - 1]) >
           DH_HELPER_RECONNECT_WINDOW_MS;
}

static bool reconnecting_repeatedly(const dh_helper *h, uint32_t now_ms) {
    return rate_reached(h->recent_drops, h->drop_count, DH_HELPER_RECONNECT_LIMIT,
                        DH_HELPER_RECONNECT_WINDOW_MS, now_ms) ||
           rate_reached(h->recent_session_losses, h->session_loss_count,
                        DH_HELPER_SESSION_LOSS_LIMIT, DH_HELPER_SESSION_LOSS_WINDOW_MS, now_ms);
}

/* The rate, with the reading that produced it. The note goes first and only
   when the state actually changes — a rate restated every cycle is noise, and
   a state without its number is what let #94 run for two days. */
static void report_repeated_reconnection(dh_helper *h, uint32_t now_ms, dh_helper_outputs *o) {
    if (h->state == DH_HELPER_RECONNECTING_REPEATEDLY) return;

    /*
     * The reading that actually tripped, not whichever ring is fuller. A slow
     * loop reported as four inside thirty seconds sends the user after a cable
     * that is fine; a fast one reported over three quarters of an hour buries
     * the number that matters. The short window is named first because where
     * both hold it is the tighter statement of the two.
     */
    if (rate_reached(h->recent_drops, h->drop_count, DH_HELPER_RECONNECT_LIMIT,
                     DH_HELPER_RECONNECT_WINDOW_MS, now_ms))
        put_note(o, DH_NOTE_RECONNECTION_RATE, (int32_t)h->drop_count,
                 (int32_t)(now_ms - h->recent_drops[0]));
    else if (h->session_loss_count > 0)
        put_note(o, DH_NOTE_RECONNECTION_RATE, (int32_t)h->session_loss_count,
                 (int32_t)(now_ms - h->recent_session_losses[0]));
    set_state(h, o, DH_HELPER_RECONNECTING_REPEATEDLY);
}

static void drop_connection(dh_helper *h, uint32_t now_ms, dh_helper_outputs *o,
                            dh_helper_note note, int32_t a, int32_t b) {
    /*
     * Every teardown feeds the short window. This one feeds the long window as
     * well: it is a session *this end* gave up on, and nothing has said the
     * device went anywhere — which is the whole of what the long window
     * measures, and what #107 spent sixteen hours doing invisibly.
     */
    const bool alone = stands_alone(h, now_ms);
    if (record_drop(h, now_ms) && alone)
        push_time(h->recent_session_losses, &h->session_loss_count, DH_HELPER_SESSION_LOSS_LIMIT,
                  now_ms);
    forget_session(h);
    h->holding_channels = false;
    h->have_deferred = true;
    h->deferred_state = DH_HELPER_DEVICE_ABSENT;
    h->deferred_at = now_ms;

    put_note(o, note, a, b);
    put(o, DH_HELPER_OUT_CLOSE_CHANNELS);

    /* Only these two are falsified by the rate. A helper already reporting
       something specific — not paired, version incompatible — is not made more
       accurate by being told the link flaps as well. */
    const bool falsified = h->state == DH_HELPER_CONNECTED || h->state == DH_HELPER_QUIET;
    if (falsified && reconnecting_repeatedly(h, now_ms)) report_repeated_reconnection(h, now_ms, o);

    put_retry(o, backoff_next(h));
}

/* ------------------------------------------------------------------ entropy */

static uint64_t fresh_correlation(dh_helper *h) {
    uint8_t bytes[8] = {0};
    h->identity->entropy(h->identity->ctx, bytes, sizeof bytes);

    uint64_t value = 0;
    for (unsigned i = 0; i < 8; i++) value |= (uint64_t)bytes[i] << (i * 8u);
    return value;
}

/* --------------------------------------------------------------- the hello */

/*
 * One place for the hello, because acquisition and a fresh pairing both need
 * it. A new nonce and a new correlation value each time: the correlation is
 * what ties an answer to its question, and a reused one would let a listener's
 * provoked refusal be believed on a later attempt (#108).
 */
static bool build_hello(dh_helper *h, uint8_t *out, size_t cap, size_t *out_len,
                       dh_helper_outputs *o) {
    uint8_t nonce[DH_NONCE_SIZE] = {0};
    h->identity->entropy(h->identity->ctx, nonce, sizeof nonce);

    memcpy(h->helper_nonce, nonce, sizeof nonce);
    h->have_nonce = true;
    h->hello_correlation = fresh_correlation(h);

    uint8_t k_hello[DH_SESSION_KEY_SIZE];
    if (h->have_board_key) {
        uint8_t shared[DH_P256_SHARED_SIZE];
        if (!h->identity->ecdh(h->identity->ctx, h->board_public_key, shared)) {
            /* The pinned key is not a point on the curve. Its own note: a
               refusal to encode would say the frame was the problem. */
            put_note(o, DH_NOTE_KEY_DERIVATION_FAILED, 0, 0);
            return false;
        }
        dh_auth_derive_hello_key(shared, nonce, k_hello);
        memset(shared, 0, sizeof shared);
    } else {
        /*
         * Never paired, so there is no shared secret to key on. The board
         * checks its registration *before* it checks the tag, so a hello keyed
         * on nothing is refused with HELLO_REFUSED(unpaired) — which is the
         * answer this helper needs — rather than met with silence.
         */
        const uint8_t none[DH_P256_SHARED_SIZE] = {0};
        dh_auth_derive_hello_key(none, nonce, k_hello);
    }

    dh_hello hello = {
        .proto_version = DH_PROTO_VERSION,
        .os = h->identity->os,
        .build_type = h->identity->build_type,
        .channel_count = DH_SESSION_CHANNEL_COUNT,
        .max_chunk = DH_SESSION_MAX_CHUNK,
        .correlation = h->hello_correlation,
    };
    memcpy(hello.helper_key_id, h->identity->key_id, DH_KEY_ID_SIZE);
    memcpy(hello.helper_nonce, nonce, DH_NONCE_SIZE);

    const dh_frame_result rc = dh_hello_encode(&hello, k_hello, 0, out, cap, out_len);
    memset(k_hello, 0, sizeof k_hello);
    if (rc != DH_FRAME_OK) put_note(o, DH_NOTE_HELLO_ENCODE_FAILED, (int32_t)rc, 0);
    return rc == DH_FRAME_OK;
}

/* A fresh PAIR_REQUEST, and the correlation value a grant must echo back
   before this helper will act on it (#108). */
static bool build_pair_request(dh_helper *h, uint32_t now_ms, uint8_t *out, size_t cap,
                               size_t *out_len) {
    dh_pair_request request = {.correlation = fresh_correlation(h)};
    memcpy(request.helper_public, h->identity->public_key, DH_P256_PUBLIC_SIZE);

    if (dh_pair_request_encode(&request, out, cap, out_len) != DH_FRAME_OK) return false;

    h->pair_correlation = request.correlation;
    h->pairing_requested = true;
    h->pairing_requested_at = now_ms;
    h->last_sent_at = now_ms;
    return true;
}

/* ---------------------------------------------------------------- lifecycle */

void dh_helper_init(dh_helper *h, const dh_helper_identity *identity,
                    const uint8_t *board_public_key) {
    memset(h, 0, sizeof *h);
    h->identity = identity;
    h->state = DH_HELPER_QUIET;
    h->phase = DH_HELPER_PHASE_IDLE;
    dh_frame_reader_init(&h->reader);
    dh_auth_counter_init(&h->rx);
    backoff_reset(h);
    /* Both directions until the board says otherwise, matching the stored
       default. Zeroing here would silently refuse every copy on a board that
       has not got as far as stating a policy. */
    h->clip_flags = DH_CLIP_MAY_SEND | DH_CLIP_MAY_RECEIVE;

    if (board_public_key != NULL) {
        memcpy(h->board_public_key, board_public_key, DH_P256_PUBLIC_SIZE);
        h->have_board_key = true;
    }
}

bool dh_helper_device_drops(const dh_helper *h, dh_device_drops *out) {
    if (!h->have_device_drops) return false;
    *out = h->device_drops;
    return true;
}

void dh_helper_set_payload_sink(dh_helper *h, dh_helper_payload_fn fn, void *ctx) {
    h->payload_fn = fn;
    h->payload_ctx = ctx;
}

/* The clock arrives with the first input, because a machine with no clock of
   its own cannot ask what time it started. */
static void note_started(dh_helper *h, uint32_t now_ms) {
    if (h->started) return;
    h->started = true;
    h->started_at = now_ms;
}

/* ----------------------------------------------------------- device presence */

static void device_left(dh_helper *h, dh_helper_state reason, uint32_t now_ms,
                        dh_helper_outputs *o) {
    (void)record_drop(h, now_ms);
    /*
     * The long window starts again. It holds sessions lost while the board
     * stayed attached, and the board has just left — so an unplug, a sleep, or
     * a config-mode round trip cannot accumulate there, however many of them a
     * day carries. Unconditional, and not inside record_drop: a disappearance
     * the debounce swallows is still a disappearance.
     */
    h->session_loss_count = 0;
    forget_session(h);
    h->have_deferred = true;
    h->deferred_state = reason;
    h->deferred_at = now_ms;

    if (h->holding_channels) {
        h->holding_channels = false;
        put(o, DH_HELPER_OUT_CLOSE_CHANNELS);
    }
}

void dh_helper_device_appeared(dh_helper *h, dh_device_identity which, uint32_t now_ms,
                               dh_helper_outputs *o) {
    note_started(h, now_ms);
    /*
     * Keyed on *any* identity appearing, not on the normal one. Config mode
     * reboots the device under a different identity, and a helper started
     * during that window would otherwise leave the never-attached fallback
     * armed and decay to "device not connected" five seconds in — #73, fixed
     * in 61e9127 and carried down here rather than re-derived, in one place
     * precisely so a third identity cannot reintroduce it by omission.
     */
    h->ever_saw_device = true;

    if (which == DH_DEVICE_CONFIG_MODE) {
        device_left(h, DH_HELPER_DEVICE_IN_CONFIG_MODE, now_ms, o);
        return;
    }

    h->have_deferred = false;
    backoff_reset(h);
    if (h->state == DH_HELPER_DEVICE_ABSENT || h->state == DH_HELPER_DEVICE_IN_CONFIG_MODE)
        set_state(h, o, DH_HELPER_QUIET);
    put(o, DH_HELPER_OUT_OPEN_CHANNELS);
}

void dh_helper_device_disappeared(dh_helper *h, uint32_t now_ms, dh_helper_outputs *o) {
    note_started(h, now_ms);
    device_left(h, DH_HELPER_DEVICE_ABSENT, now_ms, o);
}

/* --------------------------------------------------------------- acquisition */

void dh_helper_channels_acquired(dh_helper *h, uint8_t count, uint32_t now_ms,
                                 dh_helper_outputs *o) {
    (void)count; /* the effective count is negotiated, not counted here */
    note_started(h, now_ms);

    h->holding_channels = true;
    h->phase = DH_HELPER_PHASE_AWAITING_ACK;
    h->hello_sent_at = now_ms;
    h->last_sent_at = now_ms;
    h->last_device_frame_at = now_ms;

    uint8_t frame[DH_HELPER_FRAME_MAX];
    size_t len = 0;
    if (!build_hello(h, frame, sizeof frame, &len, o)) {
        /*
         * The channel opened and nothing can be said down it — a stored board
         * key that is not a point on the curve, or an enclave that will not
         * answer. That is a device this helper cannot use, and it is reported
         * on exactly the terms every other unusable device is, by the same
         * call: nothing at first, "device not connected" once the window has
         * passed, and the real reason in the log.
         *
         * Reported by hand here once, which was wrong twice over — the line
         * above has just cleared the deferral, so nothing re-armed it and a
         * board whose key had gone bad said **nothing at all, for ever**.
         */
        dh_helper_acquisition_refused(h, 0, 0, now_ms, o);
        return;
    }
    /* Cleared here rather than on the way in: an open channel is not yet
       evidence of a usable device, and a hello that cannot be built has to
       leave the pending report standing. */
    h->have_deferred = false;
    put_bytes(o, DH_HELPER_OUT_SEND, frame, len);

    /*
     * A handshake that keeps failing with the board answering nothing.
     *
     * #117 fixed this in the firmware: a board registered to *someone else*
     * now refuses with `unpaired` rather than falling through to the tag check
     * and staying silent, so this helper reaches "not paired" on its own and
     * the ask below never fires.
     *
     * Kept anyway, deliberately. Firmware and helper ship separately, so this
     * helper still meets boards that predate #117 — and on those, silence
     * never reaches the state that asks to be paired, which leaves the config
     * chord with nothing to provision and the machine reconnecting for ever.
     * The cost is one untagged frame per acquisition, and only once the rate
     * already says retrying is not working. A board outside a pairing window
     * ignores it.
     *
     * Gated on the last hello having gone unanswered (#107), which is what the
     * note beside it has always claimed: *the handshake is not completing*.
     * The rate alone does not say that. A session that completes every time
     * and then dies of a liveness timeout trips the same rate, and there the
     * registration is working and a fresh one fixes nothing.
     *
     * Measured on Windows: after a liveness timeout the helper asked to pair
     * while holding a valid board key, and the board refused with `already
     * registered`. Correct, and only because no window happened to be open at
     * that moment — a helper that re-pairs whenever a session dies is one that
     * walks into the next window somebody opens for a different reason.
     *
     * Gating on holding no board key was tried first and is wrong: the case
     * above is a *paired* helper whose own identity changed, so it still holds
     * the pin while the board no longer knows it.
     */
    if (h->state == DH_HELPER_RECONNECTING_REPEATEDLY && h->hello_went_unanswered &&
        build_pair_request(h, now_ms, frame, sizeof frame, &len)) {
        put_note(o, DH_NOTE_ASKING_TO_BE_PAIRED, 0, 0);
        put_bytes(o, DH_HELPER_OUT_SEND, frame, len);
    }
}

/*
 * The open failed. Under v1 this reported "another program holds the channel",
 * which on macOS asserts something that cannot happen: a second seizing open
 * succeeds, measured. The state is gone with the claim (#72, #114, ADR-0008),
 * and no state replaces it, because a refused open names no remedy of its own.
 *
 * What is left is a device this helper cannot use, reported the way every other
 * unusable device is: a deferred absence, so a failure that persists says
 * "device not connected" instead of saying nothing for ever, with the real
 * reason in the log. A partial acquisition — the ordinary shape when #63's
 * channel nodes arrive one at a time — clears the deferral on the retry that
 * completes, well inside the window.
 *
 * The deferral is armed once and **not pushed out by later refusals**. The
 * backoff caps at 4 s and the window is 5 s, so re-arming it on every retry
 * would move the deadline further away than the retries are apart, and a device
 * that could never be opened would say nothing at all, for ever — which is
 * precisely the outcome the deferral exists to prevent. The clock runs from the
 * first failure.
 */
void dh_helper_acquisition_refused(dh_helper *h, uint8_t acquired, uint8_t of, uint32_t now_ms,
                                   dh_helper_outputs *o) {
    note_started(h, now_ms);
    forget_session(h);
    h->holding_channels = false;
    if (!h->have_deferred) {
        h->have_deferred = true;
        h->deferred_state = DH_HELPER_DEVICE_ABSENT;
        h->deferred_at = now_ms;
    }

    put(o, DH_HELPER_OUT_CLOSE_CHANNELS);
    if (acquired > 0)
        put_note(o, DH_NOTE_PARTIAL_ACQUISITION, acquired, of);
    else
        put_note(o, DH_NOTE_EVERY_CHANNEL_REFUSED, 0, 0);
    put_retry(o, backoff_next(h));
}

/* -------------------------------------------------------------- the hello ack */

/*
 * HELLO_ACK is the one frame whose key is not known when it arrives: the
 * session keys need the board's nonce, and the board's nonce is inside it. So
 * the nonce is read out of the body's fixed offset before anything about the
 * frame is trusted, the keys are derived, and only then is the tag checked.
 * Nothing is committed until it verifies.
 */
static const uint8_t *peek_board_nonce(const dh_frame_view *f) {
    const size_t need = DH_FRAME_AUTH_PREFIX_SIZE + DH_HELLO_ACK_LEN;
    if (f->hdr.len < need) return NULL;
    /* The nonce is the tail of the body: correlation, version, build, count
       and chunk come first. dh_hello_ack_decode owns that layout. */
    return f->payload + DH_FRAME_AUTH_PREFIX_SIZE + (DH_HELLO_ACK_LEN - DH_NONCE_SIZE);
}

static void on_hello_ack(dh_helper *h, const dh_frame_view *f, uint32_t now_ms,
                         dh_helper_outputs *o) {
    if (h->phase != DH_HELPER_PHASE_AWAITING_ACK) {
        put_note(o, DH_NOTE_IGNORED_OUTSIDE_SESSION, f->hdr.type, 0);
        return;
    }
    if (!h->have_board_key) {
        drop_connection(h, now_ms, o, DH_NOTE_NO_BOARD_KEY, 0, 0);
        return;
    }
    if (!h->have_nonce) {
        drop_connection(h, now_ms, o, DH_NOTE_NO_STORED_NONCE, 0, 0);
        return;
    }

    const uint8_t *board_nonce = peek_board_nonce(f);
    if (board_nonce == NULL) {
        drop_connection(h, now_ms, o, DH_NOTE_ACK_TOO_SHORT, f->hdr.len, 0);
        return;
    }

    uint8_t shared[DH_P256_SHARED_SIZE];
    if (!h->identity->ecdh(h->identity->ctx, h->board_public_key, shared)) {
        drop_connection(h, now_ms, o, DH_NOTE_KEY_DERIVATION_FAILED, 0, 0);
        return;
    }

    uint8_t k_h2b[DH_SESSION_KEY_SIZE];
    uint8_t k_b2h[DH_SESSION_KEY_SIZE];
    dh_auth_derive_session_keys(shared, h->helper_nonce, board_nonce, k_h2b, k_b2h);
    memset(shared, 0, sizeof shared);

    /* A counter of its own, so a frame that does not verify cannot move the
       session's counter state before the session exists. */
    dh_auth_counter rx;
    dh_auth_counter_init(&rx);

    const uint8_t *body = NULL;
    size_t body_len = 0;
    if (dh_auth_open(k_b2h, &f->hdr, f->payload, &rx, &body, &body_len) != DH_AUTH_OK) {
        drop_connection(h, now_ms, o, DH_NOTE_TAG_FAILED, f->hdr.type, 0);
        return;
    }

    dh_hello_ack ack;
    if (!dh_hello_ack_decode(body, body_len, &ack)) {
        drop_connection(h, now_ms, o, DH_NOTE_UNDECODABLE, f->hdr.type, 0);
        return;
    }
    /* A genuine ack, for somebody else's hello. Dropped, not acted on — this
       is the half of #108 that a tag alone does not close. */
    if (ack.correlation != h->hello_correlation) {
        put_note(o, DH_NOTE_IGNORED_WRONG_CORRELATION, f->hdr.type, 0);
        return;
    }

    memcpy(h->k_h2b, k_h2b, sizeof k_h2b);
    memcpy(h->k_b2h, k_b2h, sizeof k_b2h);
    h->have_keys = true;
    h->rx = rx;
    h->tx_counter = 0;

    backoff_reset(h);
    h->phase = DH_HELPER_PHASE_LIVE;
    h->last_device_frame_at = now_ms;
    h->negotiated.channel_count = ack.channel_count;
    h->negotiated.max_chunk = ack.max_chunk;
    h->negotiated.device_build = ack.build_type;
    h->have_negotiated = true;
    /* The board answered, so whatever the rate says, it is not the handshake. */
    h->hello_went_unanswered = false;

    if (ack.build_type == DH_BUILD_DEVELOPMENT) put_note(o, DH_NOTE_DEVELOPMENT_BUILD, 0, 0);

    if (reconnecting_repeatedly(h, now_ms))
        report_repeated_reconnection(h, now_ms, o);
    else
        set_state(h, o, DH_HELPER_CONNECTED);
}

static void on_hello_refused(dh_helper *h, const dh_frame_view *f, uint32_t now_ms,
                             dh_helper_outputs *o) {
    if (h->phase != DH_HELPER_PHASE_AWAITING_ACK) {
        put_note(o, DH_NOTE_IGNORED_OUTSIDE_SESSION, f->hdr.type, 0);
        return;
    }

    dh_hello_refused refused;
    if (!dh_hello_refused_decode(f->payload, f->hdr.len, &refused) ||
        (refused.status != DH_HELLO_REFUSED_UNPAIRED &&
         refused.status != DH_HELLO_REFUSED_VERSION_INCOMPATIBLE)) {
        drop_connection(h, now_ms, o, DH_NOTE_UNDECODABLE, f->hdr.type, 0);
        return;
    }
    if (refused.correlation != h->hello_correlation) {
        put_note(o, DH_NOTE_IGNORED_WRONG_CORRELATION, f->hdr.type, 0);
        return;
    }

    backoff_reset(h);

    if (refused.status == DH_HELLO_REFUSED_UNPAIRED) {
        /*
         * A live phase with nothing negotiated: the channel is open and the
         * board is answering, there is simply no session to run on it. That is
         * what lets an unpaired helper sit there being told nothing without
         * timing itself out, and what lets the pair request go out on a tick.
         *
         * The pin stays. See dh_helper.board_public_key — dropping it here
         * would mean a swapped board is accepted silently after any restart.
         */
        h->phase = DH_HELPER_PHASE_LIVE;
        h->have_negotiated = false;
        h->pairing_requested = false;
        forget_beat_trace(h);
        forget_crypto_state(h);
        set_state(h, o, DH_HELPER_NOT_PAIRED);
        return;
    }

    put_note(o, DH_NOTE_VERSION_MISMATCH, refused.proto_version, DH_PROTO_VERSION);
    forget_session(h);
    set_state(h, o, DH_HELPER_VERSION_INCOMPATIBLE);
}

/* -------------------------------------------------------------------- pairing */

static void on_pair_grant(dh_helper *h, const dh_frame_view *f, uint32_t now_ms,
                          dh_helper_outputs *o) {
    /*
     * A grant is only ever an answer, so there must be a question outstanding.
     * The correlation check below cannot stand in for this one: the second
     * copy of a grant carries the value this helper really did ask with, and
     * acting on it tears down the live session the first copy produced —
     * re-pinning the key, sending a fresh hello, and leaving the user reading
     * "connected" over a session that has gone.
     */
    if (h->phase == DH_HELPER_PHASE_IDLE || !h->pairing_requested) {
        put_note(o, DH_NOTE_IGNORED_OUTSIDE_SESSION, f->hdr.type, 0);
        return;
    }

    dh_pair_grant grant;
    if (!dh_pair_grant_decode(f->payload, f->hdr.len, &grant)) {
        put_note(o, DH_NOTE_UNDECODABLE, f->hdr.type, 0);
        return;
    }
    if (grant.correlation != h->pair_correlation) {
        put_note(o, DH_NOTE_IGNORED_WRONG_CORRELATION, f->hdr.type, 0);
        return;
    }

    /*
     * A different board. The chord was pressed and the grant is genuine — but
     * it is a genuine grant from something that is not the board this helper
     * was paired with, and accepting it silently is how a swapped board
     * inherits the trust of the one it replaced.
     *
     * Deliberately not offered the chord: pressing it is the very act that
     * would accept the new board. The way through is for the user to remove
     * the pinned key, which says "I re-flashed it" in the one place a
     * bystander on the channel cannot reach.
     */
    if (h->have_board_key &&
        memcmp(h->board_public_key, grant.board_public, DH_P256_PUBLIC_SIZE) != 0) {
        h->pairing_requested = false;
        put_note(o, DH_NOTE_BOARD_IDENTITY_CHANGED, 0, 0);
        set_state(h, o, DH_HELPER_BOARD_IDENTITY_CHANGED);
        return;
    }

    /*
     * Held until the key has at least produced a hello. A grant carrying
     * something that is not a point on the curve must leave the helper as it
     * found it, not holding a key every later hello fails on.
     *
     * It is stored at that point rather than after the ack, because the ack is
     * what the *next* hello needs the key to verify: a helper that waited would
     * have nothing to authenticate the hello it sends after a restart.
     */
    uint8_t previous[DH_P256_PUBLIC_SIZE];
    const bool had_key = h->have_board_key;
    memcpy(previous, h->board_public_key, sizeof previous);

    memcpy(h->board_public_key, grant.board_public, DH_P256_PUBLIC_SIZE);
    h->have_board_key = true;

    uint8_t frame[DH_HELPER_FRAME_MAX];
    size_t len = 0;
    if (!build_hello(h, frame, sizeof frame, &len, o)) {
        memcpy(h->board_public_key, previous, sizeof previous);
        h->have_board_key = had_key;
        return;
    }

    h->pairing_requested = false;
    h->hello_sent_at = now_ms;
    h->last_sent_at = now_ms;
    h->phase = DH_HELPER_PHASE_AWAITING_ACK;

    put_bytes(o, DH_HELPER_OUT_STORE_BOARD_KEY, grant.board_public, DH_P256_PUBLIC_SIZE);
    put_note(o, DH_NOTE_PAIRED_BY_DEVICE, 0, 0);
    put_bytes(o, DH_HELPER_OUT_SEND, frame, len);
}

static void on_pair_refused(dh_helper *h, const dh_frame_view *f, dh_helper_outputs *o) {
    if (h->phase == DH_HELPER_PHASE_IDLE) {
        put_note(o, DH_NOTE_IGNORED_OUTSIDE_SESSION, f->hdr.type, 0);
        return;
    }

    dh_pair_refused refused;
    if (!dh_pair_refused_decode(f->payload, f->hdr.len, &refused) ||
        (refused.reason != DH_PAIR_REFUSED_NO_WINDOW &&
         refused.reason != DH_PAIR_REFUSED_ALREADY_REGISTERED)) {
        put_note(o, DH_NOTE_UNDECODABLE, f->hdr.type, 0);
        return;
    }
    if (refused.correlation != h->pair_correlation) {
        put_note(o, DH_NOTE_IGNORED_WRONG_CORRELATION, f->hdr.type, 0);
        return;
    }
    put_note(o, DH_NOTE_PAIR_REFUSED, refused.reason, 0);
}

/* ------------------------------------------------------------ session traffic */

static void on_device_beat(dh_helper *h, const dh_frame_view *f, uint32_t now_ms,
                           dh_helper_outputs *o) {
    if (h->phase == DH_HELPER_PHASE_IDLE) {
        put_note(o, DH_NOTE_IGNORED_OUTSIDE_SESSION, f->hdr.type, 0);
        return;
    }

    const uint32_t last = h->last_device_beat_at;
    const bool had = h->have_device_beat;
    h->last_device_beat_at = now_ms;
    h->have_device_beat = true;

    if (!had) {
        put_note(o, DH_NOTE_FIRST_BEAT, 0, 0);
        return;
    }
    if (!h->beat_quiet_noted) return;

    h->beat_quiet_noted = false;
    put_note(o, DH_NOTE_BEAT_RESUMED, (int32_t)(now_ms - last), 0);
}

static void on_session_end(dh_helper *h, const dh_frame_view *f, const uint8_t *body,
                           size_t body_len, uint32_t now_ms, dh_helper_outputs *o) {
    if (h->phase == DH_HELPER_PHASE_IDLE) {
        put_note(o, DH_NOTE_IGNORED_OUTSIDE_SESSION, f->hdr.type, 0);
        return;
    }
    const int32_t reason = body_len > 0 ? body[0] : DH_SESSION_END_UNSPECIFIED;
    drop_connection(h, now_ms, o, DH_NOTE_SESSION_ENDED, reason, 0);
}

/*
 * The board's clipboard direction policy (#52). Reported every time it is
 * stated, not only when it changes: the board already sends it only on a
 * change or a fresh session, and a platform that has just started has nothing
 * to compare against.
 */
static void on_clip_policy(dh_helper *h, const dh_frame_view *f, const uint8_t *body,
                           size_t body_len, dh_helper_outputs *o) {
    uint8_t flags = 0;
    if (!dh_clip_policy_decode(body, body_len, &flags)) {
        put_note(o, DH_NOTE_UNDECODABLE, f->hdr.type, 0);
        return;
    }

    h->clip_flags = flags;
    h->have_clip_policy = true;

    dh_helper_output *item = slot(o);
    if (item != NULL) {
        item->kind = DH_HELPER_OUT_CLIP_POLICY;
        item->a = (int32_t)flags;
    }
    put_note(o, DH_NOTE_CLIP_POLICY, (int32_t)flags, 0);
}

/*
 * What the board has dropped on the channel (#133). Stored rather than
 * reported: there is no single number a note could carry, and the moment these
 * are wanted is a stall, which is when the helper asks for them.
 */
static void on_device_drops(dh_helper *h, const dh_frame_view *f, const uint8_t *body,
                            size_t body_len, dh_helper_outputs *o) {
    dh_device_drops drops;
    if (!dh_device_drops_decode(body, body_len, &drops)) {
        put_note(o, DH_NOTE_UNDECODABLE, f->hdr.type, 0);
        return;
    }

    h->device_drops = drops;
    h->have_device_drops = true;
}

static void on_listener_alert(dh_helper *h, const dh_frame_view *f, const uint8_t *body,
                              size_t body_len, uint32_t now_ms, dh_helper_outputs *o) {
    dh_listener_alert alert;
    if (!dh_listener_alert_decode(body, body_len, &alert)) {
        put_note(o, DH_NOTE_UNDECODABLE, f->hdr.type, 0);
        return;
    }

    /* The alert is a rate, so it expires like one. The board measured it over a
       window and said so; if nothing further arrives within another such
       window, whatever was writing has stopped, and leaving the warning up for
       the rest of the session would make it a latch rather than a reading —
       the mistake #94 corrected for reconnections. */
    h->listener_alert_at = now_ms;
    h->listener_alert_window_ms = alert.window_ms;
    h->listener_alert_live = true;

    put_note(o, DH_NOTE_LISTENER_DETECTED, (int32_t)alert.refused, (int32_t)alert.window_ms);
    set_state(h, o, DH_HELPER_LISTENER_DETECTED);
}

/* --------------------------------------------------------------- dispatching */

/* Frames carrying an authentication prefix. Liveness is updated only when the
   tag verifies — v2's fix for #95: under v1 a second writer's traffic held the
   board's view of the helper alive after the real helper had stopped. */
static void on_authenticated(dh_helper *h, const dh_frame_view *f, uint32_t now_ms,
                             dh_helper_outputs *o) {
    if (f->hdr.type == DH_MSG_HELLO_ACK) {
        on_hello_ack(h, f, now_ms, o);
        return;
    }

    if (!h->have_keys) {
        put_note(o, DH_NOTE_NO_SESSION_KEY, f->hdr.type, 0);
        return;
    }

    const uint8_t *body = NULL;
    size_t body_len = 0;
    const dh_auth_result rc = dh_auth_open(h->k_b2h, &f->hdr, f->payload, &h->rx, &body, &body_len);
    if (rc == DH_AUTH_ERR_TAG) {
        /*
         * docs/protocol.md, "the helper's side of the same rule": only the
         * device emits device→helper reports, so a tag that fails here means
         * the board is not the board this helper paired with, or the byte
         * stream is corrupt. Either way the connection goes, and nothing is
         * replied — an answer would reach every attached client.
         */
        drop_connection(h, now_ms, o, DH_NOTE_TAG_FAILED, f->hdr.type, 0);
        return;
    }
    if (rc == DH_AUTH_ERR_COUNTER) {
        /*
         * Not the same case. The tag verified, so this came from the board; the
         * counter is merely not greater than one already accepted. The device's
         * outbound path is a bounded queue (ADR-0005) where gaps are ordinary,
         * so this costs the frame, not the session.
         */
        put_note(o, DH_NOTE_COUNTER_REPLAYED, f->hdr.type, 0);
        return;
    }
    if (rc != DH_AUTH_OK) {
        put_note(o, DH_NOTE_FRAME_DROPPED, f->hdr.type, rc);
        return;
    }

    h->last_device_frame_at = now_ms;

    switch (f->hdr.type) {
    case DH_MSG_SESSION_END:
        on_session_end(h, f, body, body_len, now_ms, o);
        break;
    case DH_MSG_DEVICE_HEARTBEAT:
        on_device_beat(h, f, now_ms, o);
        break;
    case DH_MSG_LISTENER_ALERT:
        on_listener_alert(h, f, body, body_len, now_ms, o);
        break;
    case DH_MSG_CLIP_POLICY:
        on_clip_policy(h, f, body, body_len, o);
        break;
    case DH_MSG_DEVICE_DROPS:
        on_device_drops(h, f, body, body_len, o);
        break;
    default:
        /* Placement and bulk are authenticated here — which is what makes them
           liveness — and carried by the platform, not decided by this machine.
           The sink is where "carried" happens; without one they are dropped,
           which is a helper that has no payloads rather than an error. */
        if (h->payload_fn != NULL) h->payload_fn(h->payload_ctx, f->hdr.type, body, body_len);
        break;
    }
}

/* Frames in the unauthenticated band (0x08-0x0F): pairing, and the hello
   refusals a board sends when it has no key to tag with. No liveness update —
   these are not session traffic, and treating them as such is exactly what
   would let a listener hold a dead session open. */
static void on_unauthenticated(dh_helper *h, const dh_frame_view *f, uint32_t now_ms,
                               dh_helper_outputs *o) {
    switch (f->hdr.type) {
    case DH_MSG_HELLO_REFUSED:
        on_hello_refused(h, f, now_ms, o);
        break;
    case DH_MSG_PAIR_GRANT:
        on_pair_grant(h, f, now_ms, o);
        break;
    case DH_MSG_PAIR_REFUSED:
        on_pair_refused(h, f, o);
        break;
    default:
        break;
    }
}

void dh_helper_received(dh_helper *h, const uint8_t *data, size_t len, uint32_t now_ms,
                        dh_helper_outputs *o) {
    note_started(h, now_ms);

    size_t offset = 0;
    while (offset < len) {
        size_t consumed = 0;
        dh_frame_view f;
        const dh_frame_result rc =
            dh_frame_reader_push(&h->reader, data + offset, len - offset, &consumed, &f);
        offset += consumed;

        if (rc == DH_FRAME_AGAIN) break;
        if (rc != DH_FRAME_OK) {
            drop_connection(h, now_ms, o, DH_NOTE_PROTOCOL_ERROR, (int32_t)rc, 0);
            return;
        }

        if (dh_msg_is_authenticated(f.hdr.type))
            on_authenticated(h, &f, now_ms, o);
        else
            on_unauthenticated(h, &f, now_ms, o);
    }
}

void dh_helper_transport_failed(dh_helper *h, uint32_t now_ms, dh_helper_outputs *o) {
    note_started(h, now_ms);
    drop_connection(h, now_ms, o, DH_NOTE_TRANSPORT_FAILED, 0, 0);
}

dh_frame_result dh_helper_emit(dh_helper *h, uint8_t type, uint8_t flags, const uint8_t *body,
                               size_t body_len, uint8_t *out, size_t cap, size_t *out_len) {
    if (!dh_helper_can_send_bulk(h) || !h->have_keys) return DH_FRAME_ERR_UNKNOWN_TYPE;

    const dh_frame_result rc =
        dh_auth_frame(type, flags, h->k_h2b, h->tx_counter, body, body_len, out, cap, out_len);
    /* Spent whether or not the caller gets the frame out: a receiver refuses
       anything not strictly greater, so a gap is ordinary and a reused counter
       is not. */
    if (rc == DH_FRAME_OK) h->tx_counter++;
    return rc;
}

void dh_helper_note_sent(dh_helper *h, uint32_t now_ms) {
    note_started(h, now_ms);
    h->last_sent_at = now_ms;
}

/* ----------------------------------------------------------------------- tick */

void dh_helper_tick(dh_helper *h, uint32_t now_ms, dh_helper_outputs *o) {
    note_started(h, now_ms);

    if (h->have_deferred) {
        if (elapsed(now_ms, h->deferred_at, DH_HELPER_SILENCE_MS)) {
            h->have_deferred = false;
            set_state(h, o, h->deferred_state);
        }
    } else if (!h->ever_saw_device && elapsed(now_ms, h->started_at, DH_HELPER_SILENCE_MS)) {
        /* Nothing has ever attached. The deferral covers a device that went
           away; this covers one that was never there. */
        set_state(h, o, DH_HELPER_DEVICE_ABSENT);
    }

    if (h->phase == DH_HELPER_PHASE_AWAITING_ACK) {
        if (elapsed(now_ms, h->hello_sent_at, DH_HELPER_HELLO_TIMEOUT_MS)) {
            /* The one drop a pairing window can do anything about: the board
               said nothing, which on firmware predating #117 is how it answers
               a helper it has no registration for. */
            h->hello_went_unanswered = true;
            drop_connection(h, now_ms, o, DH_NOTE_NO_ACK, DH_HELPER_HELLO_TIMEOUT_MS, 0);
        }
        return;
    }
    if (h->phase != DH_HELPER_PHASE_LIVE) return;

    /*
     * Liveness, scoped to a session. An unpaired helper — live phase, nothing
     * negotiated — has no session to time out, and timing it out would drop the
     * channel it is waiting to be paired on.
     */
    if (h->have_negotiated && elapsed(now_ms, h->last_device_frame_at, DH_SESSION_ABSENT_MS)) {
        drop_connection(h, now_ms, o, DH_NOTE_DEVICE_SILENT, DH_SESSION_ABSENT_MS, 0);
        return;
    }

    /* The repeated reconnection aged out — the link is holding. */
    if (h->state == DH_HELPER_RECONNECTING_REPEATEDLY && h->have_negotiated &&
        !reconnecting_repeatedly(h, now_ms))
        set_state(h, o, DH_HELPER_CONNECTED);

    /* The listener alert aged out — nothing further was refused. */
    if (h->state == DH_HELPER_LISTENER_DETECTED && h->have_negotiated && h->listener_alert_live &&
        elapsed(now_ms, h->listener_alert_at, h->listener_alert_window_ms)) {
        h->listener_alert_live = false;
        set_state(h, o, DH_HELPER_CONNECTED);
    }

    /* The beat stopped while the session did not. Scoped to a session for the
       same reason liveness is. */
    if (h->have_negotiated && h->have_device_beat && !h->beat_quiet_noted &&
        elapsed(now_ms, h->last_device_beat_at, DH_SESSION_ABSENT_MS)) {
        h->beat_quiet_noted = true;
        put_note(o, DH_NOTE_BEAT_QUIET, (int32_t)(now_ms - h->last_device_beat_at), 0);
    }

    /*
     * The heartbeat fills a direction that has carried nothing for a full
     * interval — ADR-0004. Any traffic at all suppresses it, which is what
     * stops a busy transfer being evicted mid-flight by the machinery that
     * exists to notice a dead one.
     */
    if (h->have_keys && elapsed(now_ms, h->last_sent_at, DH_SESSION_HEARTBEAT_MS)) {
        uint8_t frame[DH_HELPER_FRAME_MAX];
        size_t len = 0;
        if (dh_auth_frame(DH_MSG_HEARTBEAT, 0, h->k_h2b, h->tx_counter, NULL, 0, frame,
                          sizeof frame, &len) == DH_FRAME_OK) {
            h->tx_counter++;
            h->last_sent_at = now_ms;
            put_bytes(o, DH_HELPER_OUT_SEND, frame, len);
        }
    }

    /* Unpaired: ask again, periodically. Untagged, so no key is needed. A board
       outside a pairing window answers with silence. */
    if (h->state == DH_HELPER_NOT_PAIRED &&
        (!h->pairing_requested ||
         elapsed(now_ms, h->pairing_requested_at, DH_HELPER_PAIRING_RETRY_MS))) {
        uint8_t frame[DH_HELPER_FRAME_MAX];
        size_t len = 0;
        if (build_pair_request(h, now_ms, frame, sizeof frame, &len))
            put_bytes(o, DH_HELPER_OUT_SEND, frame, len);
    }
}
