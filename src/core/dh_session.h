/*
 * deskhopplus shared core — the session layer (#45).
 *
 * Two things live here, both above framing (dh_frame.h) and below any
 * transport:
 *
 *   1. The hello / hello_ack payload codecs, used by *both* ends. The device
 *      answers with them and each helper asks with them, so the negotiated
 *      fields cannot drift between three implementations.
 *   2. The device's side of the session: version check, negotiation of the
 *      channel count and chunk size, and heartbeat liveness — the helper is
 *      marked absent after a couple of missed intervals.
 *
 * Authentication (#46) is enforced here and decided in dh_pair.h: a hello
 * carrying no valid token is answered DH_HELLO_AUTH_FAILED, which is a
 * different status from a version mismatch because the remedies differ and
 * each is told to the user verbatim. A development build compiles the check
 * out and says so in its build type.
 *
 * Pure C11, no I/O, no platform dependencies. Wire format: docs/protocol.md;
 * test-vectors/frames.txt is the gate.
 */

#ifndef DH_SESSION_H_
#define DH_SESSION_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dh_frame.h"
#include "dh_pair.h"
#include "dh_xfer.h"

#define DH_PROTO_VERSION 1u

/*
 * Shipped as one channel (ADR-0002): the count is negotiated in the hello
 * rather than baked into the wire format, so raising it is a descriptor edit
 * and a different number here — never a protocol revision.
 */
#define DH_SESSION_CHANNEL_COUNT 1u

/*
 * Liveness, symmetric and independently timed per direction (ADR-0004).
 * Either end treats its peer as present while *anything at all* has arrived
 * within DH_SESSION_ABSENT_MS — two missed intervals plus one of grace for a
 * beat already on the wire. Traffic is the measurement; the heartbeat only
 * fills a direction that has carried nothing for a full interval, so silence
 * is unambiguous.
 *
 * The gating is not a micro-optimisation. The device holds one outbound frame
 * slot, shared with relayed bulk, and a refused queue is a silent loss — an
 * unconditional beat would be starved by a sustained transfer, and a few
 * starved beats look exactly like a dead session. The mechanism would
 * manufacture the failure it exists to detect.
 */
#define DH_SESSION_HEARTBEAT_MS 1000u
#define DH_SESSION_MISSED_INTERVALS 2u

/*
 * An enum constant rather than a computed macro so the bindings import the
 * value itself: a helper that had to re-derive `missed + 1` in its own
 * language would be a third copy of the rule, free to drift from the two
 * ends it is supposed to keep measuring the same thing.
 */
enum { DH_SESSION_ABSENT_MS = DH_SESSION_HEARTBEAT_MS * (DH_SESSION_MISSED_INTERVALS + 1u) };

/*
 * The chunk size the device offers. It is DH_XFER_CHUNK_SIZE — the transfer
 * core's working build constant (#39 measures, then this is what changes) —
 * negotiated down to whatever a helper asks for, and floored so that a
 * helper asking for nothing does not end up with an unusable session.
 */
#define DH_SESSION_MAX_CHUNK DH_XFER_CHUNK_SIZE
#define DH_SESSION_MIN_CHUNK 64u

/* Payload sizes, per docs/protocol.md. A hello's token is the remainder. */
#define DH_HELLO_FIXED_LEN 7u
#define DH_HELLO_ACK_LEN 7u
#define DH_HELLO_TOKEN_MAX 64u
#define DH_SESSION_END_LEN 1u

/*
 * Why a session ended, told to the helper so it need not wait out its own
 * timeout. Nothing about the *remedy* differs — every reason drops the
 * connection and reconnects — so unlike dh_hello_status these are diagnostic,
 * not behavioural, and an unknown value reads as unspecified rather than as
 * an error. That is what lets a later device end a session for a reason a
 * shipped helper predates.
 */
typedef enum {
    DH_SESSION_END_UNSPECIFIED = 0,
    DH_SESSION_END_LIVENESS_TIMEOUT = 1,
    DH_SESSION_END_PROTOCOL_ERROR = 2,
    DH_SESSION_END_RE_PAIRED = 3,
} dh_session_end_reason;

typedef enum {
    DH_OS_MAC = 1,
    DH_OS_WINDOWS = 2,
} dh_os;

typedef enum {
    DH_BUILD_RELEASE = 0,
    DH_BUILD_DEVELOPMENT = 1,
} dh_build_type;

/*
 * Distinguishable because the remedies differ, and each is told to the user
 * verbatim: re-pair with the config chord, versus update the helper.
 */
typedef enum {
    DH_HELLO_OK = 0,
    DH_HELLO_AUTH_FAILED = 1,
    DH_HELLO_VERSION_INCOMPATIBLE = 2,
} dh_hello_status;

/* Helper → device. channel_count and max_chunk are what the helper asks for. */
typedef struct {
    uint16_t proto_version;
    uint8_t os;
    uint8_t build_type;
    uint8_t channel_count;
    uint16_t max_chunk;
    const uint8_t *token; /* views the caller's payload; may be NULL */
    uint16_t token_len;
} dh_hello;

/* Device → helper. The fields are the *effective* values, zeroed on failure. */
typedef struct {
    uint16_t proto_version;
    uint8_t status;
    uint8_t build_type;
    uint8_t channel_count;
    uint16_t max_chunk;
} dh_hello_ack;

/*
 * Payload codecs. Decode reads a frame's payload; encode writes a complete
 * frame (header included) so a caller never assembles one by hand.
 */
bool dh_hello_decode(const uint8_t *payload, size_t len, dh_hello *out);
dh_frame_result dh_hello_encode(const dh_hello *in, uint8_t *out, size_t cap, size_t *out_len);
bool dh_hello_ack_decode(const uint8_t *payload, size_t len, dh_hello_ack *out);
dh_frame_result dh_hello_ack_encode(const dh_hello_ack *in, uint8_t *out, size_t cap,
                                    size_t *out_len);

/* The device's view of the one helper on its side of the link. */
typedef struct {
    bool present;
    uint8_t build_type;  /* this device's own, reported in every ack */
    uint8_t peer_os;     /* DH_OS_*, valid while present */
    uint8_t channel_count; /* effective, negotiated; 0 until a session exists */
    uint16_t max_chunk;    /* effective, negotiated */
    uint32_t last_seen_ms;
    /* When this device last had something to say. The beat fills the gaps
       between real traffic rather than adding to it. */
    uint32_t last_sent_ms;
    /* Authenticated peers are the only ones anything is relayed for. */
    bool authenticated;
} dh_session;

/*
 * build_type is the device's own — a development build must identify itself
 * (#44), and the helper reads it out of every ack.
 */
void dh_session_init(dh_session *s, uint8_t build_type);

/*
 * Feed one decoded frame. When a reply is owed it is encoded as a complete
 * frame into out and *out_len is its size; otherwise *out_len is 0 and the
 * result is DH_FRAME_OK. Frames outside the session band are not this layer's
 * business and are ignored with no reply.
 */
dh_frame_result dh_session_on_frame(dh_session *s, dh_pair *pair, const dh_frame_view *f,
                                    uint32_t now_ms, uint8_t *out, size_t out_cap,
                                    size_t *out_len);

/*
 * May anything be relayed for this peer? False until a hello authenticated —
 * the device relays nothing for an unauthenticated peer (#34), and that is
 * one check rather than a second state machine.
 */
static inline bool dh_session_may_relay(const dh_session *s) {
    return s->present && s->authenticated;
}

/*
 * Something went out to the helper. Feeds the idle timer the device's own
 * beat runs on: any frame already proves this device alive and holding a
 * session, so the beat is owed only when nothing else was sent.
 *
 * Called on a successful queue rather than a successful send — a frame the
 * transport accepted is one the helper will see, and the distinction is
 * beneath this layer.
 */
void dh_session_note_sent(dh_session *s, uint32_t now_ms);

/*
 * Something arrived from the helper. Any well-formed frame proves the sender
 * is alive, so any frame refreshes the deadline — including bulk, which is
 * relayed opaquely and never reaches dh_session_on_frame, so the transport
 * has to hand it here itself.
 *
 * This is the mirror of dh_session_note_sent, and both ends must apply it, or
 * they disagree about what liveness is: a helper that suppresses its beat
 * because it is busy sending, talking to a device that only counts beats,
 * is a helper evicted in the middle of its own traffic.
 *
 * Only a helper that already has a session — silence is what ends one, and
 * a stray frame must not start one.
 */
void dh_session_note_received(dh_session *s, uint32_t now_ms);

/*
 * Advance the clock, encoding whatever is owed the helper into out — the
 * device's beat when the direction has been idle for an interval, or a
 * SESSION_END on the call that evicts a helper for silence. Never both: an
 * evicted session is not beaten at. *out_len is 0 when nothing is due.
 *
 * There is no separate transition flag. The eviction *is* the frame, so the
 * signal and what goes on the wire cannot disagree.
 *
 * Time is uint32_t milliseconds and comparisons are wrap-safe.
 */
dh_frame_result dh_session_tick(dh_session *s, uint32_t now_ms, uint8_t *out, size_t out_cap,
                                size_t *out_len);

/*
 * End the session and say so — for the evictions this device decides on and
 * can still write about: a framing error on its reader, and the config chord
 * rotating the secret. Encodes a SESSION_END into out.
 *
 * A session that was never established emits nothing (*out_len is 0): a
 * helper that never had one must not be told that one of its ended.
 */
dh_frame_result dh_session_end(dh_session *s, uint8_t reason, uint8_t *out, size_t out_cap,
                               size_t *out_len);

/*
 * The link dropped underneath us — no timeout needs to elapse, and no
 * announcement is possible or wanted: there is nobody on the other end to
 * hear it. dh_session_end is the form for an eviction this device chose.
 */
void dh_session_drop(dh_session *s);

#endif /* DH_SESSION_H_ */
