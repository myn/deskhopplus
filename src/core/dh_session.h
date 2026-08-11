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
 * Liveness. The helper beats every DH_SESSION_HEARTBEAT_MS; the device marks
 * it absent once two intervals have gone by with nothing heard, plus one
 * interval of grace for a beat already on the wire.
 */
#define DH_SESSION_HEARTBEAT_MS 1000u
#define DH_SESSION_MISSED_INTERVALS 2u
#define DH_SESSION_ABSENT_MS (DH_SESSION_HEARTBEAT_MS * (DH_SESSION_MISSED_INTERVALS + 1u))

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
 * Advance the clock. Returns true on the call that marks a present helper
 * absent, so a caller can act on the transition rather than poll the flag.
 * Time is uint32_t milliseconds and comparisons are wrap-safe.
 */
bool dh_session_tick(dh_session *s, uint32_t now_ms);

/* The link dropped underneath us — no timeout needs to elapse. */
void dh_session_drop(dh_session *s);

#endif /* DH_SESSION_H_ */
