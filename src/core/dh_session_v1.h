/*
 * deskhopplus shared core — the v1 hello codecs, parked for the macOS helper.
 *
 * PARKED, NOT LIVE. The board speaks v2 (dh_session.h, ADR-0008, #111). This
 * file exists only because the macOS helper still speaks v1 until #112 gives
 * it a Secure Enclave identity and the v2 handshake, and it binds straight to
 * these codecs. Nothing in the firmware includes it.
 *
 * DELETE THIS FILE IN #112, together with dh_session_v1.c, the v1 names in
 * helpers/macos/Sources/DeskhopChannel/SessionMessages.swift, and the frozen
 * v1 frames in helpers/macos/Tests/channel-tests/BindingTests.swift. Nothing
 * new is written against v1, so the two protocols cannot drift.
 *
 * The consequence of the gap is stated plainly, because it is not a bug: a
 * v1 helper cannot pair with a v2 board. ADR-0008 accepted exactly that —
 * "old pairings do not migrate", and recovery is one chord press. There is no
 * migration path because a migration path would have to accept the bearer
 * token, which is the thing being removed.
 *
 * Pure C11, no I/O, no platform dependencies.
 */

#ifndef DH_SESSION_V1_H_
#define DH_SESSION_V1_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dh_frame.h"

/*
 * v1's version constant. It is frozen at 1 for good: the number moves with
 * the code, and the code that moved is dh_session.c, which is now v2.
 */
#define DH_PROTO_VERSION_V1 1u

/* v1's bearer token: the length of the device-held secret, and of the token a
   v1 hello carries. There is no v2 equivalent — nothing secret crosses. */
#define DH_PAIR_SECRET_LEN 16u

/*
 * Payload sizes. A v1 hello's token is the remainder.
 *
 * Every name here carries the `_V1` suffix, and `DH_HELLO_ACK_LEN` is the
 * reason: v2 defines that name as 30. The firmware and these two .c files are
 * separate translation units and would never have noticed, but SwiftPM builds
 * src/core as **one clang module** with `publicHeadersPath: "."`, so both
 * headers land in the same include set — and the v1 value, imported second,
 * is the one Swift would see. Nothing reaches for it today; the suffix is what
 * stops something reaching for it tomorrow and silently sizing a v2 ack at 7
 * bytes. It also makes the deletion in #112 a grep.
 */
#define DH_HELLO_FIXED_LEN_V1 7u
#define DH_HELLO_ACK_LEN_V1 7u
#define DH_HELLO_TOKEN_MAX_V1 64u

/*
 * v1's hello statuses. Status 1 is the one v2 removed outright: a board that
 * answers a failed authentication tells every attached client that the real
 * helper is unpaired, which is how #108 manufactured the chord trap. v2's
 * board is silent instead, and reserves the value so no v2 status can be
 * misread as this one.
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
} dh_hello_v1;

/* Device → helper. The fields are the *effective* values, zeroed on failure. */
typedef struct {
    uint16_t proto_version;
    uint8_t status;
    uint8_t build_type;
    uint8_t channel_count;
    uint16_t max_chunk;
} dh_hello_ack_v1;

/*
 * Payload codecs. Decode reads a frame's payload; encode writes a complete
 * frame (header included) so a caller never assembles one by hand.
 */
bool dh_hello_v1_decode(const uint8_t *payload, size_t len, dh_hello_v1 *out);
dh_frame_result dh_hello_v1_encode(const dh_hello_v1 *in, uint8_t *out, size_t cap,
                                   size_t *out_len);
bool dh_hello_ack_v1_decode(const uint8_t *payload, size_t len, dh_hello_ack_v1 *out);
dh_frame_result dh_hello_ack_v1_encode(const dh_hello_ack_v1 *in, uint8_t *out, size_t cap,
                                       size_t *out_len);

#endif /* DH_SESSION_V1_H_ */
