/*
 * deskhopplus shared core — the *helper's* side of the session (#79, #80).
 *
 * The board's side is dh_session.h. This is the other end of the same
 * conversation: the hello exchange, negotiation, ADR-0004's liveness, the
 * pairing exchange, all-or-nothing acquisition, the capped reconnection
 * backoff, and the states a user is shown.
 *
 * It existed in exactly one place before this file — SessionEngine.swift, 503
 * lines only macOS can run — and #49 needs the same machine on Windows.
 * Writing it a second time in C++ would give ADR-0004's traffic-gated
 * liveness two implementations to get right, failing differently on two
 * operating systems under load, in a way that looks like a hardware fault.
 * So it is written once, here, and each helper is a transport and a face.
 *
 * ---------------------------------------------------------------------------
 * WHAT IS HERE AND WHAT IS NOT
 *
 * **Here:** every decision. The state values, both policy predicates, the
 * timings, the backoff, the negotiation, and the frames that go out.
 *
 * `dh_helper_prompts_config_chord` is the one that is not a presentation
 * choice. The chord provisions whatever is attached to the channel during its
 * window (#34), so a state that offers it while something else holds the
 * channel hands that something else the pairing. It is decided once, not per
 * platform.
 *
 * **Not here:** the wording. A Windows tray tooltip and a macOS menu bar item
 * are not one string table living in C — every output carries a code and its
 * numbers, and each helper says it in its own words. That is also why this
 * file needs no stdio.
 *
 * Also not here: secret *storage*. The decision to store a board key is an
 * output (DH_HELPER_OUT_STORE_BOARD_KEY); whether it lands in DPAPI or a 0600
 * file is the platform's business.
 * ---------------------------------------------------------------------------
 *
 * Pure C11: no I/O, no allocation, no clock and no entropy source of its own —
 * time arrives as a millisecond argument and entropy through a callback, so
 * liveness is tested by driving ticks rather than by sleeping.
 */

#ifndef DH_HELPER_H_
#define DH_HELPER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dh_auth.h"
#include "dh_frame.h"
#include "dh_p256.h"
#include "dh_session.h"

/* ------------------------------------------------------------------ timings */

/*
 * All milliseconds, all compared with unsigned differences, so a wrapping
 * counter is arithmetic rather than a session dropped once every 49 days.
 *
 * The two that are protocol, not policy, come from dh_session.h and are not
 * restated: DH_SESSION_HEARTBEAT_MS is how long a direction may stay idle
 * before this end fills it, and DH_SESSION_ABSENT_MS is how long the board may
 * stay silent before this end gives up on the session.
 */

/*
 * How long an unusable device is given before the user is told. A device that
 * disappears for a moment is ordinary USB noise; one that is still gone five
 * seconds later is worth reporting.
 */
#define DH_HELPER_SILENCE_MS 5000u

/* A hello with no answer is a dead session, not a slow one. */
#define DH_HELPER_HELLO_TIMEOUT_MS 2000u

/*
 * The reconnection *rate*, which is what says the thing no single cycle can.
 * A helper failing every frame it received once spent two days reporting
 * "Connected and paired" because each cycle on its own was correctly too brief
 * to mention (#94).
 */
#define DH_HELPER_RECONNECT_WINDOW_MS 30000u
#define DH_HELPER_RECONNECT_LIMIT 4u

/* How often an unpaired helper asks again. The board ignores it outside a
   pairing window, so the cost of asking is one untagged frame. */
#define DH_HELPER_PAIRING_RETRY_MS 2000u

/*
 * Reconnection delay: doubling, capped. The cap is what matters — a config
 * mode round trip can take five minutes, and a helper that had backed off to
 * minutes would leave the user staring at a dead menu bar long after the
 * device came back.
 */
#define DH_HELPER_BACKOFF_FIRST_MS 250u
#define DH_HELPER_BACKOFF_CAP_MS 4000u

/* ------------------------------------------------------------------- states */

/*
 * What the user is told, named with its remedy (#38). The wording lives in
 * each helper; only the distinctions live here.
 *
 * `channelHeld` — "another program holds the channel" — used to be one of
 * these and is gone (#72, #114, ADR-0008). It asserted something that can
 * never be true on macOS: a second seizing open succeeds, measured on
 * 2026-08-13, so the open is never refused for the reason the message named.
 * What replaced it is DH_HELPER_LISTENER_DETECTED, which is measured rather
 * than assumed — the board counts frames it could not authenticate and says
 * so.
 */
typedef enum {
    /* Looking, or briefly gone. Nothing is shown: a device that disappears for
       a moment is ordinary, and config mode is something the user did. */
    DH_HELPER_QUIET = 0,
    DH_HELPER_CONNECTED = 1,
    /* The connection keeps having to be rebuilt. See the reconnect window. */
    DH_HELPER_RECONNECTING_REPEATEDLY = 2,
    DH_HELPER_NOT_PAIRED = 3,
    DH_HELPER_DEVICE_IN_CONFIG_MODE = 4,
    DH_HELPER_DEVICE_ABSENT = 5,
    DH_HELPER_VERSION_INCOMPATIBLE = 6,
    /*
     * Something other than this helper is writing frames the board could not
     * authenticate, at a rate the board measured and reported (#111). It says
     * only that: a listener that merely *reads* writes nothing to refuse and
     * cannot be detected at all, so silence here is not a clean channel.
     */
    DH_HELPER_LISTENER_DETECTED = 7,
    /*
     * The board granted a pairing under a different identity key from the one
     * this helper pinned — a board wiped past its identity sector, re-flashed,
     * or swapped (#112).
     */
    DH_HELPER_BOARD_IDENTITY_CHANGED = 8,
} dh_helper_state;

/*
 * The chord remedy, offered from exactly one state.
 *
 * The two states a chord press would make *worse* are the reason this is a
 * function and not a reading of the enum: DH_HELPER_LISTENER_DETECTED, where
 * something else is writing to the channel and the chord would provision it,
 * and DH_HELPER_BOARD_IDENTITY_CHANGED, where pressing it is the act that
 * accepts a swapped board.
 */
static inline bool dh_helper_prompts_config_chord(dh_helper_state s) {
    return s == DH_HELPER_NOT_PAIRED;
}

/*
 * An incompatible peer keeps cursor placement and refuses bulk: a misparsed
 * placement puts the cursor somewhere wrong and self-corrects, while a
 * misparsed chunk header writes a corrupted file presented as valid.
 *
 * A connection that keeps being rebuilt is not one of those — while it is up
 * it is a negotiated session like any other. Nor is a detected listener: the
 * session is authenticated, and what the board saw is somebody *writing*
 * frames the tag already keeps out. What a listener can still do is *read* a
 * payload in clear, and the remedy for that is sealing it (#113), not
 * withholding it on a signal a passive listener never trips.
 *
 * This must keep agreeing with dh_helper_can_send_bulk, the seam #52 consumes.
 */
static inline bool dh_helper_allows_bulk(dh_helper_state s) {
    return s == DH_HELPER_CONNECTED || s == DH_HELPER_RECONNECTING_REPEATEDLY ||
           s == DH_HELPER_LISTENER_DETECTED;
}

/* ------------------------------------------------------------------ outputs */

typedef enum {
    /*
     * The board's public key, out of a PAIR_GRANT this helper asked for.
     * Storing it is the platform's job; the machine only decides it is worth
     * keeping — and never emits one for a board whose key has changed.
     */
    DH_HELPER_OUT_STORE_BOARD_KEY = 0,
    DH_HELPER_OUT_OPEN_CHANNELS = 1,
    DH_HELPER_OUT_CLOSE_CHANNELS = 2,
    DH_HELPER_OUT_SEND = 3,
    DH_HELPER_OUT_STATE = 4,
    DH_HELPER_OUT_RETRY = 5, /* `a` is the delay in milliseconds */
    DH_HELPER_OUT_NOTE = 6,  /* diagnostics, never shown to the user */
} dh_helper_output_kind;

/*
 * Diagnostic codes. Their wording belongs to each helper; what they must not
 * lose is the number beside them — a note that says a rate without saying the
 * rate is the mistake #94 cost two days to.
 *
 * `a` and `b` carry those numbers, documented per code.
 */
typedef enum {
    DH_NOTE_NONE = 0,
    /* a = frame type */
    DH_NOTE_IGNORED_OUTSIDE_SESSION = 1,
    /* a = frame type. A listener can provoke a genuine refusal, but it carries
       the listener's correlation value and this is where it stops. */
    DH_NOTE_IGNORED_WRONG_CORRELATION = 2,
    DH_NOTE_UNDECODABLE = 3,          /* a = frame type */
    DH_NOTE_HELLO_ENCODE_FAILED = 4,  /* a = dh_frame_result */
    DH_NOTE_ASKING_TO_BE_PAIRED = 5,
    DH_NOTE_PARTIAL_ACQUISITION = 6,  /* a = acquired, b = of */
    DH_NOTE_EVERY_CHANNEL_REFUSED = 7,
    DH_NOTE_PROTOCOL_ERROR = 8,       /* a = dh_frame_result */
    DH_NOTE_NO_SESSION_KEY = 9,       /* a = frame type */
    DH_NOTE_TAG_FAILED = 10,          /* a = frame type */
    DH_NOTE_COUNTER_REPLAYED = 11,    /* a = frame type */
    DH_NOTE_FRAME_DROPPED = 12,       /* a = frame type, b = dh_auth_result */
    DH_NOTE_NO_BOARD_KEY = 13,
    DH_NOTE_NO_STORED_NONCE = 14,
    DH_NOTE_ACK_TOO_SHORT = 15,
    DH_NOTE_KEY_DERIVATION_FAILED = 16,
    DH_NOTE_DEVELOPMENT_BUILD = 17,   /* channel authentication is compiled out */
    DH_NOTE_VERSION_MISMATCH = 18,    /* a = the board's version, b = this helper's */
    DH_NOTE_BOARD_IDENTITY_CHANGED = 19,
    DH_NOTE_PAIRED_BY_DEVICE = 20,
    DH_NOTE_PAIR_REFUSED = 21,        /* a = dh_pair_refused_reason */
    DH_NOTE_FIRST_BEAT = 22,
    DH_NOTE_BEAT_RESUMED = 23,        /* a = ms since the last beat */
    DH_NOTE_BEAT_QUIET = 24,          /* a = ms since the last beat */
    DH_NOTE_SESSION_ENDED = 25,       /* a = dh_session_end_reason */
    DH_NOTE_LISTENER_DETECTED = 26,   /* a = refused frames, b = window ms */
    DH_NOTE_NO_ACK = 27,              /* a = ms waited */
    DH_NOTE_DEVICE_SILENT = 28,       /* a = ms of silence */
    DH_NOTE_TRANSPORT_FAILED = 29,
    DH_NOTE_RECONNECTION_RATE = 30,   /* a = drops counted, b = ms they spanned */
} dh_helper_note;

/*
 * The largest frame this machine emits: a PAIR_REQUEST, which is a 4-byte
 * header and a 72-byte body with no authentication prefix. Stated because it
 * sizes the output slot — under v1 a pair frame was 20 bytes, and a buffer
 * built on that number would silently stop this helper ever asking to be
 * paired (the shape of the defect #109 found on the board's reply buffer).
 */
#define DH_HELPER_FRAME_MAX (DH_FRAME_HEADER_SIZE + DH_PAIR_REQUEST_LEN)

typedef struct {
    uint8_t kind;  /* dh_helper_output_kind */
    uint8_t state; /* dh_helper_state, for DH_HELPER_OUT_STATE */
    uint8_t note;  /* dh_helper_note, for DH_HELPER_OUT_NOTE */
    /* Signed because some of them are result codes, which are negative. */
    int32_t a;
    int32_t b;
    /* DH_HELPER_OUT_SEND: a complete frame. DH_HELPER_OUT_STORE_BOARD_KEY:
       the board's 64-byte public key. Otherwise empty. */
    uint8_t bytes[DH_HELPER_FRAME_MAX];
    size_t len;
} dh_helper_output;

/*
 * Enough for any single input. The worst case is one received report holding
 * two frames whose handling both drops the connection; `overflow` counts what
 * would not fit rather than letting a dropped `send` pass for nothing having
 * happened.
 */
#define DH_HELPER_OUTPUTS_MAX 16u

typedef struct {
    size_t count;
    size_t overflow;
    dh_helper_output items[DH_HELPER_OUTPUTS_MAX];
} dh_helper_outputs;

void dh_helper_outputs_reset(dh_helper_outputs *o);

/* ----------------------------------------------------------------- identity */

/*
 * This helper's key pair, abstracted because the private half may be
 * unreachable: on macOS it lives in the Secure Enclave and cannot be handed to
 * C at all. What the enclave *can* do is one ECDH, so that is the whole seam.
 *
 * The HKDF over the result stays in the core (dh_auth.h). A helper deriving
 * its own session keys would be a second implementation of the rule both ends
 * must agree on, which is what this file exists to prevent.
 */
typedef struct {
    void *ctx; /* handed back to every callback; may be NULL */
    uint8_t public_key[DH_P256_PUBLIC_SIZE];
    uint8_t key_id[DH_KEY_ID_SIZE];

    /* Who this helper is running as, reported in every hello: dh_os, and
       dh_build_type for the helper's own build. Neither is derivable here —
       the whole point of this file is that it does not know its platform. */
    uint8_t os;
    uint8_t build_type;

    /* ECDH(this helper's private half, board_public) -> shared_secret.
       False when the board's key is not a point on the curve. */
    bool (*ecdh)(void *ctx, const uint8_t board_public[DH_P256_PUBLIC_SIZE],
                 uint8_t shared_secret[DH_P256_SHARED_SIZE]);

    /* Unpredictable bytes: nonces and correlation values. A core with no
       entropy source of its own asks for them, the same reasoning that makes
       dh_session stage its nonce. */
    void (*entropy)(void *ctx, uint8_t *out, size_t len);
} dh_helper_identity;

/* --------------------------------------------------------------- the machine */

typedef enum {
    /* Normal mode. The channel exists only here. */
    DH_DEVICE_NORMAL = 0,
    /*
     * Config mode reboots the device under a different USB identity for up to
     * five minutes. Seeing it tells the helper exactly what happened, which is
     * why it is a state of its own and not "the device is gone".
     */
    DH_DEVICE_CONFIG_MODE = 1,
} dh_device_identity;

typedef struct {
    uint8_t channel_count;
    uint16_t max_chunk;
    uint8_t device_build; /* dh_build_type */
} dh_helper_negotiated;

typedef enum {
    DH_HELPER_PHASE_IDLE = 0,
    DH_HELPER_PHASE_AWAITING_ACK = 1,
    DH_HELPER_PHASE_LIVE = 2,
} dh_helper_phase;

typedef struct {
    const dh_helper_identity *identity;

    /* Read these; do not write them. */
    dh_helper_state state;
    dh_helper_negotiated negotiated;
    bool have_negotiated;

    uint8_t phase; /* dh_helper_phase */
    dh_frame_reader reader;

    uint32_t backoff_ms;
    uint32_t hello_sent_at;
    uint32_t last_sent_at;
    uint32_t last_device_frame_at;
    uint32_t last_device_beat_at;
    bool have_device_beat;
    bool beat_quiet_noted;
    bool holding_channels;

    /* The last few drops, oldest first — a rate, not an event. */
    uint32_t recent_drops[DH_HELPER_RECONNECT_LIMIT];
    size_t drop_count;

    /* A state worth reporting only if it is still true when the window ends.
       `deferred_at` is when it was armed, not when it comes due: a deadline
       stored as a sum cannot be compared wrap-safely against the clock. */
    bool have_deferred;
    dh_helper_state deferred_state;
    uint32_t deferred_at;

    uint32_t pairing_requested_at;
    bool pairing_requested;

    uint32_t started_at;
    bool started;
    bool ever_saw_device;

    /*
     * The board's identity key, pinned at pairing. Deliberately **not**
     * dropped when the board says it does not know us: it is the only record
     * of which board this helper trusts, and a control a restart clears is not
     * a control. A stale pin costs nothing — the board checks its registration
     * before it checks the tag, so the hello is still refused with `unpaired` —
     * and keeping it is what lets a grant carrying a *different* key be
     * recognised as a different board (#112).
     *
     * Only the user clears it.
     */
    uint8_t board_public_key[DH_P256_PUBLIC_SIZE];
    bool have_board_key;

    /* The board's last listener alert, and the window it was measured over.
       The alert is a rate, so it expires like one; leaving the warning up for
       the rest of the session would make it a latch rather than a reading
       (#94). */
    uint32_t listener_alert_at;
    uint32_t listener_alert_window_ms;
    bool listener_alert_live;

    /* Per session, cleared together. */
    uint8_t helper_nonce[DH_NONCE_SIZE];
    bool have_nonce;
    uint64_t hello_correlation;
    uint64_t pair_correlation;
    uint8_t k_h2b[DH_SESSION_KEY_SIZE];
    uint8_t k_b2h[DH_SESSION_KEY_SIZE];
    bool have_keys;
    uint64_t tx_counter;
    dh_auth_counter rx;
} dh_helper;

/*
 * `board_public_key` is what the platform had stored, or NULL for a helper
 * that has never paired. `identity` must outlive the machine.
 */
void dh_helper_init(dh_helper *h, const dh_helper_identity *identity,
                    const uint8_t *board_public_key);

/* Whether a bulk transfer may go out right now — the seam #52 consumes. It
   answers for the *session*, where dh_helper_allows_bulk answers for what the
   user is being told; the two must not disagree. */
static inline bool dh_helper_can_send_bulk(const dh_helper *h) {
    return h->phase == DH_HELPER_PHASE_LIVE && h->have_negotiated;
}

/*
 * The inputs. Every one takes the current time in milliseconds and appends to
 * `out`, which the caller resets (or not, to batch a sequence).
 */
void dh_helper_device_appeared(dh_helper *h, dh_device_identity which, uint32_t now_ms,
                               dh_helper_outputs *out);
void dh_helper_device_disappeared(dh_helper *h, uint32_t now_ms, dh_helper_outputs *out);

/* Every channel was opened exclusively. A partial acquisition is not this
   input — it is dh_helper_acquisition_refused. */
void dh_helper_channels_acquired(dh_helper *h, uint8_t count, uint32_t now_ms,
                                 dh_helper_outputs *out);
void dh_helper_acquisition_refused(dh_helper *h, uint8_t acquired, uint8_t of, uint32_t now_ms,
                                   dh_helper_outputs *out);

/* Bytes off the channel, in order. Frame boundaries never align with report
   boundaries, so this is fed whatever arrived. */
void dh_helper_received(dh_helper *h, const uint8_t *data, size_t len, uint32_t now_ms,
                        dh_helper_outputs *out);

/*
 * The transport could not carry something it was given. A frame written in
 * part leaves the device's reader mid-frame, where the padding skip does not
 * apply and the next frame is eaten as its tail — so this is a dropped
 * connection, not a retryable write.
 */
void dh_helper_transport_failed(dh_helper *h, uint32_t now_ms, dh_helper_outputs *out);

/*
 * Build one authenticated frame to send to the board — a bulk chunk, a cursor
 * placement — under the session key and the next counter in its space.
 *
 * The counter space belongs to the key (dh_auth.h), and the heartbeat is
 * already writing into this one. A platform keeping a counter of its own
 * beside it would give one space two writers, and the board refuses anything
 * not strictly greater — so whichever frame lost the race would be dropped
 * silently, at the far end, with nothing at either end able to say why. The
 * allocation lives here for that reason and not for tidiness.
 *
 * `out` is the caller's, because a bulk chunk is far larger than anything this
 * machine emits on its own.
 *
 * DH_FRAME_ERR_UNKNOWN_TYPE when there is no session: no keys, so nothing can
 * be tagged, and no negotiated session to carry it. Mirrors
 * dh_session_emit_relayed at the other end.
 *
 * The idle timer is **not** charged here. A frame the transport then refused
 * would have suppressed a beat that was owed; call dh_helper_note_sent when it
 * actually went out.
 */
dh_frame_result dh_helper_emit(dh_helper *h, uint8_t type, uint8_t flags, const uint8_t *body,
                               size_t body_len, uint8_t *out, size_t cap, size_t *out_len);

/*
 * The platform wrote a frame this machine did not produce — a bulk chunk, a
 * cursor placement. ADR-0004's heartbeat fills a direction that has carried
 * *nothing* for a full interval, and traffic the machine never saw would
 * otherwise have it beating into a direction that is far from idle.
 */
void dh_helper_note_sent(dh_helper *h, uint32_t now_ms);

void dh_helper_tick(dh_helper *h, uint32_t now_ms, dh_helper_outputs *out);

#endif /* DH_HELPER_H_ */
