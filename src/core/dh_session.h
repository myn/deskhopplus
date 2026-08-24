/*
 * deskhopplus shared core — the session layer, protocol v2 (#111, ADR-0008).
 *
 * Two things live here, both above framing (dh_frame.h) and below any
 * transport:
 *
 *   1. The body codecs of every session-band message, used by *both* ends, so
 *      the negotiated fields cannot drift between three implementations.
 *   2. The board's side of the session: the hello exchange, the pairing
 *      exchange, per-frame authentication, heartbeat liveness, and the
 *      listener alert.
 *
 * ---------------------------------------------------------------------------
 * WHAT V2 CHANGED, AND WHY EACH CHANGE IS HERE
 *
 * **Authorisation is per frame, not per session.** v1 gated relay on one flag
 * for the whole board (`dh_session_may_relay`), so while the legitimate helper
 * held an authenticated session, any other process on the Mac could write a
 * bulk frame and the board relayed it to the other computer — the isolation
 * breach #34 exists to prevent, reachable without ever learning the secret.
 * Every client writes into the same endpoint and the board cannot tell them
 * apart, so a session flag could never have answered "who sent this frame".
 * A tag can. That flag is gone; there is nothing left to gate.
 *
 * **The board is silent when a tag does not verify.** v1 answered
 * DH_HELLO_AUTH_FAILED, that answer reached every attached client because it
 * is an input report, and the real helper read it as "not paired — press the
 * config chord". #108 measured that on hardware: one frame was enough and the
 * state was sticky. Answering is acting, so a failed tag is answered with
 * nothing at all. Two refusals *are* still sent — HELLO_REFUSED(unpaired) and
 * PAIR_REFUSED — because neither can be provoked into saying anything false: a
 * board holding no registration *for the key id a hello names* really is
 * unpaired as far as that asker is concerned, and a board with no window really
 * will not pair. That first test is per key id rather than per board (#117), so
 * that a board registered to *someone else* says so instead of falling silent —
 * silence never reaches the helper state that asks to be paired, which left the
 * config chord with nothing to provision. docs/protocol.md weighs the widening
 * against the "safe to overhear" argument.
 *
 * **Every answer echoes the caller's correlation value.** A listener can still
 * provoke a genuine refusal, but it carries the *listener's* value and the real
 * helper drops it. Belt and braces with the silence above, because the failure
 * they prevent is a user being told to hand over their pairing.
 *
 * **No asymmetric work per session.** The keys are HKDF over the pairing-time
 * shared secret and a fresh nonce from each side (dh_auth.h). #107 measured 586
 * session teardowns in 16 hours; an ECDH on each would be a 133 ms freeze of the
 * user's own mouse about once every 98 seconds.
 * ---------------------------------------------------------------------------
 *
 * Pure C11, no I/O, no clock, no entropy source. Wire format: docs/protocol.md;
 * test-vectors/frames.txt is the gate.
 */

#ifndef DH_SESSION_H_
#define DH_SESSION_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dh_auth.h"
#include "dh_frame.h"
#include "dh_pair.h"
#include "dh_xfer.h"

/* C++ links these symbols too — the Windows helper is C++ (#49). */
#ifdef __cplusplus
extern "C" {
#endif

/*
 * 2, and it moved with this file rather than with the specification. A board
 * that announced version 2 while speaking v1 would be worse than one
 * announcing the version it actually speaks — which is why #109 wrote
 * docs/protocol.md against 2 and #110 landed the primitives, and neither
 * touched this constant.
 */
#define DH_PROTO_VERSION 2u

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
 * v2 narrows "anything at all" to **frames that authenticate**, which is the
 * fix for what v1 could not express. Under v1 the deadline measured "something
 * is writing", and on macOS a second writer's traffic would hold the board's
 * view of the helper alive after the real helper had stopped (#95). A frame
 * carrying a good tag under the session key came from the holder of the
 * registered key, so the deadline now measures what it always claimed to.
 *
 * The gating of the board's own beat is not a micro-optimisation. The board's
 * outbound path is a short bounded queue (ADR-0005), and a frame it cannot
 * take is a silent loss — an unconditional beat would be starved by a
 * sustained transfer, and a few starved beats look exactly like a dead
 * session. The mechanism would manufacture the failure it exists to detect.
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
 * The chunk size the board offers. It is DH_XFER_CHUNK_SIZE — the transfer
 * core's working build constant (#39 measures, then this is what changes) —
 * negotiated down to whatever a helper asks for, and floored so that a
 * helper asking for nothing does not end up with an unusable session.
 */
#define DH_SESSION_MAX_CHUNK DH_XFER_CHUNK_SIZE
#define DH_SESSION_MIN_CHUNK 64u

/*
 * The ceiling docs/protocol.md puts on the negotiated chunk, whatever a helper
 * asks for. A sealed chunk spends 64 bytes of the 4096-byte payload maximum on
 * overhead — 24 of authentication prefix, 20 of clear chunk header, 4 of
 * CRC32, 16 of GCM tag — so a helper that asks for 4096 must be clamped, not
 * honoured. The board offers 1024 and never reaches it; the constant exists so
 * that raising DH_XFER_CHUNK_SIZE cannot quietly walk past the limit.
 */
#define DH_SESSION_CHUNK_CEILING 4032u
_Static_assert(DH_SESSION_MAX_CHUNK <= DH_SESSION_CHUNK_CEILING,
               "the offered chunk must leave room for a sealed frame's overhead");

/* Body sizes, per docs/protocol.md. Bodies, not payloads: everything outside
   types 0x08-0x0F carries the 24-byte authentication prefix in front. */
#define DH_HELLO_LEN 39u
#define DH_HELLO_ACK_LEN 30u
#define DH_LISTENER_ALERT_LEN 8u
#define DH_SESSION_END_LEN 1u
#define DH_PAIR_REQUEST_LEN (8u + DH_P256_PUBLIC_SIZE)
#define DH_PAIR_GRANT_LEN (8u + DH_P256_PUBLIC_SIZE)
#define DH_PAIR_REFUSED_LEN 9u
#define DH_HELLO_REFUSED_LEN 11u

/*
 * The largest complete frame this layer emits: a PAIR_GRANT, 4 header bytes
 * and a 72-byte body with no prefix. 76 bytes.
 *
 * Stated as a constant because it is what the board's reply buffer must hold.
 * Under v1 the grant was 20 bytes and the buffer was report-sized; a v2 board
 * built on 64 bytes would take DH_FRAME_ERR_BUFFER from dh_frame_encode and
 * therefore **never answer a pairing request**, with no error anywhere and no
 * way to tell why pairing did not work (#109).
 */
#define DH_SESSION_REPLY_MAX (DH_FRAME_HEADER_SIZE + DH_PAIR_GRANT_LEN)

/*
 * Why a session ended, told to the helper so it need not wait out its own
 * timeout. Nothing about the *remedy* differs — every reason drops the
 * connection and reconnects — so unlike the hello refusals these are
 * diagnostic, not behavioural, and an unknown value reads as unspecified
 * rather than as an error. That is what lets a later board end a session for a
 * reason a shipped helper predates.
 */
typedef enum {
    DH_SESSION_END_UNSPECIFIED = 0,
    DH_SESSION_END_LIVENESS_TIMEOUT = 1,
    DH_SESSION_END_PROTOCOL_ERROR = 2,
    /* The board no longer holds the registration this session authenticated
       against: the configuration it lived in was wiped (#75). The one reason a
       user *caused*, and the only evidence at the desk that the wipe revoked
       anything at the moment it was pressed. */
    DH_SESSION_END_UNPAIRED = 3,
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
 * Why a hello was refused. Distinguishable because the remedies differ and
 * each is told to the user verbatim.
 *
 * Value 1 is reserved and never sent. It was v1's `auth_failed`, and it is
 * left as a hole rather than reused so that no v2 status can be misread as
 * that one by anything still carrying v1's table. A failed tag has no status
 * in v2 at all: the board says nothing.
 */
typedef enum {
    DH_HELLO_REFUSED_RESERVED = 1,
    DH_HELLO_REFUSED_VERSION_INCOMPATIBLE = 2,
    DH_HELLO_REFUSED_UNPAIRED = 3,
} dh_hello_refused_status;

/* Why a pairing request was refused. Mirrors dh_pair_result on the wire. */
typedef enum {
    DH_PAIR_REFUSED_NO_WINDOW = 0,
    DH_PAIR_REFUSED_ALREADY_REGISTERED = 1,
} dh_pair_refused_reason;

/* Helper → board. channel_count and max_chunk are what the helper asks for. */
typedef struct {
    uint16_t proto_version;
    uint8_t os;
    uint8_t build_type;
    uint8_t channel_count;
    uint16_t max_chunk;
    uint64_t correlation;
    uint8_t helper_key_id[DH_KEY_ID_SIZE];
    uint8_t helper_nonce[DH_NONCE_SIZE];
} dh_hello;

/* Board → helper, on success only: there is no failure form. */
typedef struct {
    uint64_t correlation; /* echoed */
    uint16_t proto_version;
    uint8_t build_type;
    uint8_t channel_count; /* effective, negotiated */
    uint16_t max_chunk;    /* effective, negotiated */
    uint8_t board_nonce[DH_NONCE_SIZE];
} dh_hello_ack;

/* Board → helper, untagged: the board has no key to tag with, or must not use
   the one it has. */
typedef struct {
    uint64_t correlation; /* echoed */
    uint16_t proto_version; /* the board's own */
    uint8_t status;         /* dh_hello_refused_status */
} dh_hello_refused;

/* Helper → board, untagged. Only public halves cross. */
typedef struct {
    uint64_t correlation;
    uint8_t helper_public[DH_P256_PUBLIC_SIZE];
} dh_pair_request;

/* Board → helper, untagged. Nothing secret: the shared secret is computed
   independently at each end and never crosses. */
typedef struct {
    uint64_t correlation; /* echoed */
    uint8_t board_public[DH_P256_PUBLIC_SIZE];
} dh_pair_grant;

typedef struct {
    uint64_t correlation; /* echoed */
    uint8_t reason;       /* dh_pair_refused_reason */
} dh_pair_refused;

/* Board → helper. A rate, not an event — the shape #94 established for
   `reconnectingRepeatedly`, because no single frame can say what this says. */
typedef struct {
    uint32_t window_ms;
    uint32_t refused;
} dh_listener_alert;

/*
 * Body codecs. Decode reads a frame's **body** — what dh_auth_open hands back
 * for an authenticated type, or the whole payload for types 0x08-0x0F. Encode
 * writes a complete frame, header included, so a caller never assembles one by
 * hand; the authenticated forms take the key and the counter, because the
 * counter space belongs to the key.
 */
bool dh_hello_decode(const uint8_t *body, size_t len, dh_hello *out);
dh_frame_result dh_hello_encode(const dh_hello *in, const uint8_t key[DH_SESSION_KEY_SIZE],
                                uint64_t counter, uint8_t *out, size_t cap, size_t *out_len);

bool dh_hello_ack_decode(const uint8_t *body, size_t len, dh_hello_ack *out);
dh_frame_result dh_hello_ack_encode(const dh_hello_ack *in, const uint8_t key[DH_SESSION_KEY_SIZE],
                                    uint64_t counter, uint8_t *out, size_t cap, size_t *out_len);

bool dh_hello_refused_decode(const uint8_t *body, size_t len, dh_hello_refused *out);
dh_frame_result dh_hello_refused_encode(const dh_hello_refused *in, uint8_t *out, size_t cap,
                                        size_t *out_len);

bool dh_pair_request_decode(const uint8_t *body, size_t len, dh_pair_request *out);
dh_frame_result dh_pair_request_encode(const dh_pair_request *in, uint8_t *out, size_t cap,
                                       size_t *out_len);

bool dh_pair_grant_decode(const uint8_t *body, size_t len, dh_pair_grant *out);
dh_frame_result dh_pair_grant_encode(const dh_pair_grant *in, uint8_t *out, size_t cap,
                                     size_t *out_len);

bool dh_pair_refused_decode(const uint8_t *body, size_t len, dh_pair_refused *out);
dh_frame_result dh_pair_refused_encode(const dh_pair_refused *in, uint8_t *out, size_t cap,
                                       size_t *out_len);

bool dh_listener_alert_decode(const uint8_t *body, size_t len, dh_listener_alert *out);

/*
 * How wide a window the listener count is measured over, and how many refused
 * frames inside one make it worth telling the helper about.
 *
 * A firmware constant, owned by #111 — not a wire-format fact. The wire
 * carries the window and the count, so a board may change its mind about the
 * threshold without any helper changing.
 *
 * Ten seconds and four frames: low enough that a listener probing the channel
 * trips it quickly, high enough that a single corrupt report — a bus glitch, a
 * helper restarted mid-frame — does not. A rate says what no single event can.
 */
#define DH_LISTENER_WINDOW_MS 10000u
#define DH_LISTENER_THRESHOLD 4u

/* The board's view of the one helper on its side of the link. */
typedef struct {
    bool present;
    uint8_t build_type;    /* this board's own, reported in every ack */
    uint8_t peer_os;       /* DH_OS_*, valid while present */
    uint8_t channel_count; /* effective, negotiated; 0 until a session exists */
    uint16_t max_chunk;    /* effective, negotiated */
    uint32_t last_seen_ms;
    /* When this board last had something to say. The beat fills the gaps
       between real traffic rather than adding to it. */
    uint32_t last_sent_ms;

    /* The session keys, live only while present. */
    uint8_t k_h2b[DH_SESSION_KEY_SIZE];
    uint8_t k_b2h[DH_SESSION_KEY_SIZE];
    dh_auth_counter rx;  /* the highest counter accepted under k_h2b */
    uint64_t tx_counter; /* the next to send under k_b2h */

    /* The board nonce staged for the next hello. A core with no entropy
       source cannot draw one, so the caller stages it — the same reasoning
       that makes a private key 32 bytes handed in. */
    uint8_t staged_nonce[DH_NONCE_SIZE];
    bool nonce_staged;

    /* Listener detection: frames that failed authentication, in a window. */
    uint32_t window_started_ms;
    uint32_t refused_in_window;
    bool window_started;
    /* A hello naming the registered helper's key id but failing its tag. One
       of those is the stronger signal, so it alone closes the window loudly:
       something is claiming to be the registered helper and cannot prove it. */
    bool refused_as_registered;
    /* Measured, waiting for a session to tell. A board with no session has
       nobody to tell, so it keeps counting and reports on the next session
       that authenticates, carrying the window it was measured over. */
    bool alert_pending;
    dh_listener_alert alert;
} dh_session;

/*
 * build_type is the board's own — a development build must identify itself
 * (#44), and the helper reads it out of every ack.
 */
void dh_session_init(dh_session *s, uint8_t build_type);

/*
 * Fresh entropy for the board nonce of the next hello. True when the staged
 * one has been spent, so a caller can draw only when a draw is owed rather
 * than on every tick.
 */
static inline bool dh_session_needs_nonce(const dh_session *s) { return !s->nonce_staged; }
void dh_session_stage_nonce(dh_session *s, const uint8_t nonce[DH_NONCE_SIZE]);

/*
 * Feed one decoded frame from this board's helper. When a reply is owed it is
 * encoded as a complete frame into out and *out_len is its size; otherwise
 * *out_len is 0 and the result is DH_FRAME_OK.
 *
 * out must hold DH_SESSION_REPLY_MAX bytes.
 *
 * Frames the tag does not authenticate draw **nothing** and are counted. A
 * frame outside the session band is not this layer's business and is ignored
 * with no reply — the bulk band goes through dh_session_authenticate instead,
 * because it is relayed rather than acted on.
 */
dh_frame_result dh_session_on_frame(dh_session *s, dh_pair *pair, const dh_frame_view *f,
                                    uint32_t now_ms, uint8_t *out, size_t out_cap,
                                    size_t *out_len);

/*
 * Authenticate one frame this layer does not answer — the bulk band, relayed
 * opaquely to the peer helper, and the two authenticated types a helper sends
 * that draw no reply.
 *
 * On DH_AUTH_OK the body is handed back, ready to be re-framed for the
 * inter-board link, and liveness is noted. Anything else is neither acted on
 * nor relayed.
 *
 * A frame that fails its **tag or its counter** is counted towards the
 * listener alert. A frame that arrives with no session, or one too short to
 * carry a prefix at all, is not: those fail for other reasons, and counting
 * them would trip the alert on the most ordinary event on this channel — a
 * helper still beating into a session the board evicted for silence.
 *
 * This is the whole of "authorisation is per frame". A board holding a live
 * authenticated session still drops a bulk frame that does not carry a good
 * tag, which is the isolation breach #34 exists to prevent and v1 left open.
 */
dh_auth_result dh_session_authenticate(dh_session *s, const dh_frame_view *f, uint32_t now_ms,
                                       const uint8_t **body, size_t *body_len);

/*
 * Tag a frame arriving from the peer board for this board's helper, under
 * k_b2h and the next counter in that space.
 *
 * The tag is **per hop**: the far helper's tag was verified by the far board
 * and means nothing here, so what crosses the inter-board link is the frame's
 * body and this board writes a fresh tag over it. `f` is therefore a frame
 * with no authentication prefix — header and body only.
 *
 * DH_FRAME_ERR_UNKNOWN_TYPE when there is no session: no keys, so nothing can
 * be tagged, and there is no helper to send it to either.
 */
dh_frame_result dh_session_emit_relayed(dh_session *s, const dh_frame_view *f, uint8_t *out,
                                        size_t out_cap, size_t *out_len);

/*
 * Something went out to the helper. Feeds the idle timer the board's own beat
 * runs on: any frame already proves this board alive and holding a session, so
 * the beat is owed only when nothing else was sent.
 */
void dh_session_note_sent(dh_session *s, uint32_t now_ms);

/*
 * The frame dh_session_tick produced reached the outbound queue.
 *
 * Only the listener alert needs this, and the asymmetry with the beat is the
 * reason it exists rather than being folded into dh_session_note_sent. A beat
 * the queue refused is replaced by another one a second later, so charging the
 * timer for one this layer merely *produced* is right. **Nothing follows an
 * alert.** A measurement the queue refused would be thrown away, and the
 * window it was taken over cannot be measured again.
 *
 * The refusal is not hypothetical: the outbound queue's priority band holds one
 * frame, and the tick that first has a session to report an alert to is the
 * tick right after the HELLO_ACK went into that band. Until this is called, a
 * pending alert is re-encoded on the next tick — bounded, because a queue that
 * never drains is a helper that times out inside three seconds.
 */
void dh_session_note_owed_sent(dh_session *s, uint8_t type);

/*
 * Advance the clock, encoding whatever is owed the helper into out — a
 * listener alert that has been waiting for a session, the board's beat when
 * the direction has been idle for an interval, or a SESSION_END on the call
 * that evicts a helper for silence. Never more than one: an evicted session is
 * not beaten at, and one frame per tick is what the outbound queue takes.
 *
 * There is no separate transition flag. The eviction *is* the frame, so the
 * signal and what goes on the wire cannot disagree.
 *
 * Time is uint32_t milliseconds and comparisons are wrap-safe.
 */
dh_frame_result dh_session_tick(dh_session *s, uint32_t now_ms, uint8_t *out, size_t out_cap,
                                size_t *out_len);

/*
 * End the session and say so — for the evictions this board decides on and can
 * still write about: a framing error on its reader, and a wipe that took the
 * registration. Encodes a SESSION_END into out.
 *
 * A session that was never established emits nothing (*out_len is 0): a helper
 * that never had one must not be told that one of its ended.
 */
dh_frame_result dh_session_end(dh_session *s, uint8_t reason, uint8_t *out, size_t out_cap,
                               size_t *out_len);

/*
 * The link dropped underneath us — no timeout needs to elapse, and no
 * announcement is possible or wanted: there is nobody on the other end to
 * hear it. dh_session_end is the form for an eviction this board chose.
 *
 * The listener count survives, because the listener does: a client that keeps
 * writing junk across a reconnection is exactly what the alert is for.
 */
void dh_session_drop(dh_session *s);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DH_SESSION_H_ */
