/*
 * What the *other* board is running, tracked from the heartbeats it sends
 * (#89).
 *
 * Every heartbeat already carries the sender's firmware version — that is how
 * `handle_heartbeat_msg` decides whether to pull an upgrade from a newer peer.
 * It used to compare that version and then discard it, so the one question the
 * upgrade mechanism exists to answer, *is my peer on a different build?*,
 * could not be asked of either board. Answering it meant reading the local
 * version through the config UI on each computer separately, which on a
 * managed laptop is the difference between a check and an errand.
 *
 * Two facts and two operations, kept here rather than in the UART handler so
 * a test can reach them — the same split, for the same reason, as
 * config_store.c (#74).
 *
 * Pure C11: no SDK, no I/O, no clock of its own — the caller supplies the
 * time. tests/peer_fw_test.c is the gate.
 */
#pragma once

#include <stdint.h>

/*
 * No peer has been heard from: it is unplugged, the inter-board link is down,
 * or this board has only just come up.
 *
 * Zero is safe as the sentinel because no firmware can legitimately report it:
 * a version is `major * 1000 + minor + 100`, so the smallest one that exists
 * is 100. It must also be *displayed* as an absence rather than converted —
 * the config UI derives major and minor by subtracting that same 100, so a
 * zero rendered as a version reads as a negative one.
 */
#define PEER_FW_UNKNOWN 0u

/*
 * Three heartbeat intervals of silence and the peer is forgotten. Heartbeats
 * go out at 1 Hz, so this is two missed beats plus one of grace for a beat
 * already on the wire — the same discipline as the channel's liveness window
 * (ADR-0004), deliberately not the same constant: DH_SESSION_ABSENT_MS counts
 * a different link at a cadence the helper negotiates, and tying the two
 * together would make one link's timing a hostage to the other's.
 *
 * A version left reading as current after the peer has gone answers "are both
 * boards on the same build?" with a confident lie, which is worse than the
 * absence it replaced.
 */
#define PEER_FW_STALE_US (3ull * 1000000ull)

typedef struct {
    uint16_t version;     // PEER_FW_UNKNOWN when nothing has been heard
    uint32_t checksum;    // Its running image's CRC32, 0 alongside UNKNOWN
    uint64_t heard_at_us; // When the two above arrived
} peer_fw_t;

/*
 * A heartbeat arrived carrying the peer's version and image checksum. Replaces
 * whatever was held — a peer that upgrades under us reports its new build, and
 * that is the answer, not a duplicate to ignore.
 *
 * The checksum is here for the case the version cannot describe. Since #91 two
 * boards can differ at the *same* version, which is the whole point of that
 * feature — and it is exactly then that `version` answers "is my peer on a
 * different build?" with a confident yes-they-match. The heartbeat has carried
 * the checksum since #91; this only stops discarding it.
 *
 * Zero is not a sentinel for the checksum the way PEER_FW_UNKNOWN is for the
 * version — a real image could hash to it, with probability 2^-32. It is
 * cleared to zero on expiry and displayed as an absence, so that one image in
 * four billion would read as "not detected" while its version read correctly.
 * Cosmetic, and confined to the config UI: nothing in the upgrade path reads
 * this field, which keeps the display's rounding error out of the pull
 * decision. `handle_heartbeat_msg` compares the wire value directly.
 */
void peer_fw_record(peer_fw_t *peer, uint16_t version, uint32_t checksum, uint64_t now_us);

/* Forget a peer that has gone quiet — both facts, so a stale checksum cannot
   outlive the version it belongs to. Called periodically; does nothing while
   heartbeats keep arriving, since each one refreshes the timestamp. */
void peer_fw_expire(peer_fw_t *peer, uint64_t now_us);
