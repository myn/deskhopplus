/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Derek Reynolds */

#include "words.h"

#include <cstdio>

#include "dh_frame.h"
#include "dh_version.h"

namespace deskhop::words {

std::string release_row() {
    return "deskhopplus helper " + std::to_string(DH_VERSION_MAJOR) + "." +
           std::to_string(DH_VERSION_MINOR);
}

namespace {

std::string seconds(int32_t ms) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1fs", static_cast<double>(ms) / 1000.0);
    return buf;
}

/* The same, for a span that reaches into minutes — which the slow reconnection
   window does (#107). Three quarters of an hour printed in tenths of a second
   is the number, but not one anybody reads at a glance. */
std::string span(int32_t ms) {
    if (ms < 60000) return seconds(ms);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f min", static_cast<double>(ms) / 60000.0);
    return buf;
}

std::string hex_byte(int32_t value) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "frame of type 0x%02x", static_cast<unsigned>(value) & 0xFFu);
    return buf;
}

std::string lowered(const char *symbol) {
    std::string out;
    for (const char *p = symbol; *p; ++p)
        out.push_back(static_cast<char>(*p >= 'A' && *p <= 'Z' ? *p - 'A' + 'a' : *p));
    return out;
}

/*
 * The protocol's own spelling for a frame type, so a log line and
 * docs/protocol.md name the same thing.
 *
 * Generated from the core's registry rather than typed out again: a table of
 * twenty-odd names copied by hand is a table that goes stale the first time a
 * message is added, and the registry is already the single list the enum and
 * the known-type check are both built from.
 */
std::string type_name(int32_t type) {
    switch (static_cast<uint8_t>(type)) {
#define DH_MSG_NAME_CASE(name, value)                                                    \
    case (value):                                                                        \
        /* "DH_MSG_HELLO_ACK" is 7 characters of prefix, then the wire's own name. */    \
        return lowered(&(#name)[7]);
        DH_MSG_TYPE_LIST(DH_MSG_NAME_CASE)
#undef DH_MSG_NAME_CASE
    default:
        return hex_byte(type);
    }
}

std::string pair_refused(int32_t reason) {
    switch (static_cast<dh_pair_refused_reason>(reason)) {
    case DH_PAIR_REFUSED_NO_WINDOW:
        return "pairing refused: no window open";
    case DH_PAIR_REFUSED_ALREADY_REGISTERED:
        return "pairing refused: board already has a registration";
    default:
        return "pairing refused: reason " + std::to_string(reason);
    }
}

std::string session_end(int32_t reason) {
    switch (static_cast<dh_session_end_reason>(reason)) {
    case DH_SESSION_END_UNSPECIFIED:
        return "unspecified";
    case DH_SESSION_END_LIVENESS_TIMEOUT:
        return "liveness timeout";
    case DH_SESSION_END_PROTOCOL_ERROR:
        return "protocol error";
    case DH_SESSION_END_UNPAIRED:
        return "this helper is not paired";
    case DH_SESSION_END_STREAM_GAP:
        /* Reserved; nothing sends it since #188. Named for what the board
           lost, not for a fault at this end (#161). */
        return "the board lost a report, so the stream had a gap in it";
    default:
        return "reason " + std::to_string(reason);
    }
}

} // namespace

std::string state_message(dh_helper_state state) {
    switch (state) {
    case DH_HELPER_QUIET:
        return {};
    case DH_HELPER_CONNECTED:
        return "Connected and paired";
    case DH_HELPER_CONNECTED_CONFIG_MODE:
        return "Connected and paired — config mode";
    case DH_HELPER_RECONNECTING_REPEATEDLY:
        return "Reconnecting repeatedly — check the cable, and that the helper is up to date";
    case DH_HELPER_NOT_PAIRED:
        return "Not paired — press the config chord on the device";
    case DH_HELPER_DEVICE_IN_CONFIG_MODE:
        return "Device in config mode";
    case DH_HELPER_DEVICE_ABSENT:
        return "Device not connected";
    case DH_HELPER_VERSION_INCOMPATIBLE:
        return "Helper version does not match the device — file transfers are refused";
    case DH_HELPER_LISTENER_DETECTED:
        return "Another program is writing to the device channel — find and stop it, "
               "and do not press the config chord while it is running";
    case DH_HELPER_BOARD_IDENTITY_CHANGED:
        return "Device identity changed — if you re-flashed it, remove the pinned board key";
    case DH_HELPER_STATE_COUNT:
        break; /* a bound, never a state */
    }
    return {};
}

Look look(dh_helper_state state, bool question_waiting) {
    /* A question is the one thing here that is not a state, and it outranks
       every state: a transfer that can take minutes must be agreed to, and a
       badge nobody sees is a transfer that never happens (#56). */
    if (question_waiting) return Look::Attention;
    switch (state) {
    case DH_HELPER_CONNECTED:
    case DH_HELPER_CONNECTED_CONFIG_MODE:
        return Look::Paired;
    case DH_HELPER_QUIET:
    case DH_HELPER_DEVICE_ABSENT:
    case DH_HELPER_DEVICE_IN_CONFIG_MODE:
        return Look::Off;
    case DH_HELPER_RECONNECTING_REPEATEDLY:
    case DH_HELPER_NOT_PAIRED:
    case DH_HELPER_VERSION_INCOMPATIBLE:
    case DH_HELPER_LISTENER_DETECTED:
    case DH_HELPER_BOARD_IDENTITY_CHANGED:
        return Look::Attention;
    case DH_HELPER_STATE_COUNT:
        break; /* a bound, never a state */
    }
    return Look::Off;
}

std::string tooltip(dh_helper_state state, const std::string &question_summary,
                    uint64_t received, uint64_t total, bool sending) {
    std::string tip;
    if (!question_summary.empty()) {
        tip = "Files offered: " + question_summary;
    } else if (total > 0) {
        tip = "Receiving " + size_text(received) + " of " + size_text(total) + " \xe2\x80\x94 " +
              std::to_string(received * 100u / total) + "%";
    } else if (sending) {
        tip = "Sending";
    } else {
        tip = state_message(state);
        /* The quiet state has no words of its own, and since #208 it has an
           icon to hover over, so it borrows the menu's. */
        if (tip.empty()) tip = "Looking for the device";
    }
    return "deskhopplus \xe2\x80\x94 " + tip;
}

std::string size_text(uint64_t bytes) {
    if (bytes >= 1024u * 1024u) {
        const uint64_t tenths = (bytes * 10u) / (1024u * 1024u);
        return std::to_string(tenths / 10u) + "." + std::to_string(tenths % 10u) + " MB";
    }
    if (bytes >= 1024u) return std::to_string(bytes / 1024u) + " KB";
    return std::to_string(bytes) + " bytes";
}

bool state_is_known(dh_helper_state state) {
    /* Quiet has no words on purpose, so an empty message cannot be the test.
       Everything else must have some. */
    return state == DH_HELPER_QUIET || !state_message(state).empty();
}

bool state_names_a_remedy(dh_helper_state state) {
    /*
     * Four states, and each names something the user can go and do: press the
     * chord, find the other program, update one end, or clear a pinned key.
     * (#49 asked for three, listing `channelHeld` — retired by #114 — and the
     * two measured states from #111 and #112 arrived after it was written.)
     * The rest change the tooltip and the look silently: ordinary
     * reconnection is not worth interrupting anyone for, and the quiet state
     * has no words of its own (#208 gave it the off look and a borrowed
     * tooltip, not a balloon).
     *
     * This is presentation, which is why it lives here. The one predicate that
     * is *not* — whether the chord may be offered at all — is
     * dh_helper_prompts_config_chord and is called, never restated: the chord
     * provisions whatever is attached to the channel during its window (#34),
     * so it has one answer across both helpers.
     */
    return state == DH_HELPER_NOT_PAIRED || state == DH_HELPER_LISTENER_DETECTED ||
           state == DH_HELPER_VERSION_INCOMPATIBLE || state == DH_HELPER_BOARD_IDENTITY_CHANGED;
}

std::string note_line(dh_helper_note note, int32_t a, int32_t b,
                      const std::string &transport_reason) {
    switch (note) {
    case DH_NOTE_NONE:
        return "note with no code";
    case DH_NOTE_IGNORED_OUTSIDE_SESSION:
        return "ignoring a " + type_name(a) + " that arrived outside a session";
    case DH_NOTE_IGNORED_WRONG_CORRELATION:
        return "ignoring a " + type_name(a) + " with the wrong correlation value";
    case DH_NOTE_UNDECODABLE:
        return "ignoring a " + type_name(a) + " that could not be decoded";
    case DH_NOTE_HELLO_ENCODE_FAILED:
        return "the hello could not be encoded (frame result " + std::to_string(a) + ")";
    case DH_NOTE_ASKING_TO_BE_PAIRED:
        return "asking to be paired: the handshake is not completing";
    case DH_NOTE_PARTIAL_ACQUISITION:
        return "released " + std::to_string(a) + " of " + std::to_string(b) +
               " channels: a partial acquisition is not a session";
    case DH_NOTE_EVERY_CHANNEL_REFUSED:
        /* No cause claimed. The core is not told one, and this line asserting
           contention is what sent #87's sitting hunting for a second process
           during an ordinary config-mode round trip (#125). The open failure
           logged just above carries the Win32 error, which does say. */
        return "every channel refused — the open failure above says whether another program "
               "holds it or the device has gone; retrying either way";
    case DH_NOTE_PROTOCOL_ERROR:
        return "protocol error on the channel (frame result " + std::to_string(a) + ")";
    case DH_NOTE_NO_SESSION_KEY:
        return "dropping a " + type_name(a) + " with no session key";
    case DH_NOTE_TAG_FAILED:
        return "a device→helper " + type_name(a) + " failed its tag";
    case DH_NOTE_CHUNK_TAG_TOLERATED:
        /* Kept deliberately close to DH_NOTE_TAG_FAILED's wording — same fact,
           different consequence (#63): this one costs the chunk, not the
           connection, so the transfer's own retry sweep is what answers it. */
        return "a device→helper " + type_name(a) + " failed its tag; asking for it again "
               "rather than dropping the connection";
    case DH_NOTE_COUNTER_REPLAYED:
        return "dropping a " + type_name(a) + " with a counter already seen";
    case DH_NOTE_FRAME_DROPPED:
        return "dropping a " + type_name(a) + " (auth result " + std::to_string(b) + ")";
    case DH_NOTE_NO_BOARD_KEY:
        return "received a hello_ack but have no board key";
    case DH_NOTE_NO_STORED_NONCE:
        return "received a hello_ack with no stored nonce";
    case DH_NOTE_ACK_TOO_SHORT:
        return "a hello_ack of " + std::to_string(a) + " bytes is too short to carry a board nonce";
    case DH_NOTE_KEY_DERIVATION_FAILED:
        /* One code, two causes, and the log has to name both: a private key
           that will not agree and a pinned board key that is not a point on
           the curve reach the core as the same ECDH returning false. */
        return "could not derive a key against the board's — this helper's stored key would not "
               "agree, or the pinned board key is not a point on the curve";
    case DH_NOTE_DEVELOPMENT_BUILD:
        return "device is a development build: channel authentication is compiled out";
    case DH_NOTE_VERSION_MISMATCH:
        return "device speaks protocol version " + std::to_string(a) + ", this helper speaks " +
               std::to_string(b);
    case DH_NOTE_BOARD_IDENTITY_CHANGED:
        return "the board granted pairing under a different identity key — re-flashed, wiped "
               "past its identity sector, or swapped";
    case DH_NOTE_PAIRED_BY_DEVICE:
        return "paired by the device";
    case DH_NOTE_PAIR_REFUSED:
        return pair_refused(a);
    case DH_NOTE_FIRST_BEAT:
        return "device heartbeat: first beat of the session";
    case DH_NOTE_BEAT_RESUMED:
        return "device heartbeat resumed after " + seconds(a);
    case DH_NOTE_BEAT_QUIET:
        return "device heartbeat quiet for " + seconds(a);
    case DH_NOTE_BOARD_SILENT_FOR:
        return "the board says it heard nothing for " + std::to_string(a) + "ms";
    case DH_NOTE_BOARD_LOST_AT_END:
        /* Zero here is the useful reading as often as not: it says the board's
           inbound chain lost nothing, so an eviction it reported is not a
           dropped report and the cause is elsewhere (#161). */
        return "the board's inbound lost " + std::to_string(a) +
               " report(s) and " + std::to_string(b) + " peer frame(s)";
    case DH_NOTE_BOARD_HEARD_BYTES:
        /* The one reading that splits what is left: bytes arriving with no
           frame out of them is a stalled reader, and no bytes at all is a
           transport that reported a write it did not make (#161). */
        return "in the " + std::to_string(b) + "ms before that the board's USB took " +
               std::to_string(a) + " report(s)";
    case DH_NOTE_BOARD_AT_END:
        return "at the end the board had accepted " + std::to_string(a) + " frame(s) from " +
               std::to_string(b) + " report(s)";
    case DH_NOTE_BOARD_SENDS:
        /* Printed whole, zeros included. This names whole frames lost — the
           board's queue refusing them (its own refusal totals say how many)
           and frames the reader discarded around a lost report alike;
           DH_NOTE_STREAM_MISALIGNED says whether the wire took any. */
        return "the board built " + std::to_string(a) + " frame(s) for this helper; " +
               std::to_string(b) + " never arrived";
    case DH_NOTE_STREAM_MISALIGNED:
        /* The reader's resync count: reports and partial frames it threw away
           to get back to a frame start, not gaps — the lost head of a long
           chunk counts once per orphaned continuation, one line each. */
        return "a report from the board went missing; the reader resynced (" +
               std::to_string(a) + " report(s) or partial frame(s) discarded this session)";
    case DH_NOTE_LOCAL_SENDS:
        return "this helper has written " + std::to_string(a) + " frame(s) since boot, " +
               std::to_string(b) + " refused by the transport";
    case DH_NOTE_SESSION_ENDED:
        /* This end's own silence goes beside the board's reason, because on a
           liveness end the two disagreeing is the finding (#107). */
        /* A count, not a time since. "ms since the last send" was shipped
           first and read 0-3 ms on every hardware sample, because the helper
           sends in the same turn it processes the session end — it described
           the ordering, not the link. A count over the eviction window has
           nothing to trip on that way: zero means this end agrees it was
           silent, and a large number means the board heard none of what it
           sent. */
        return "the device ended the session: " + session_end(a) +
               "; this helper got " + std::to_string(b) + " frame(s) out over that window";
    case DH_NOTE_LISTENER_DETECTED:
        return "listener detected: " + std::to_string(a) + " refused frames in " +
               std::to_string(b) + "ms";
    case DH_NOTE_NO_ACK:
        return "no hello_ack within " + seconds(a);
    case DH_NOTE_DEVICE_SILENT:
        return "nothing from the device in " + seconds(a);
    case DH_NOTE_TRANSPORT_FAILED:
        return "transport failed: " +
               (transport_reason.empty() ? std::string("no reason given") : transport_reason);
    case DH_NOTE_RECONNECTION_RATE:
        return "the last " + std::to_string(a) + " reconnections came inside " + span(b);
    case DH_NOTE_CLIP_POLICY:
        /* Both verbs are named either way round — "may send / may not send"
           rather than listing only what is allowed — because a line that says
           nothing about a direction reads as that direction being untouched,
           and the whole point of this note is to say what the board decided
           about each. */
        return std::string("the board's clipboard policy: ") +
               ((a & DH_CLIP_MAY_SEND) ? "may send" : "may not send") + ", " +
               ((a & DH_CLIP_MAY_RECEIVE) ? "may receive" : "may not receive") +
               /* And the size cap beside them (#56). Its absence from this line
                  is what made a 2 MB cap unreadable in a log where a 2.5 MB
                  file was being offered — the macOS twin has printed it since
                  the cap existed. */
               ", size cap " + std::to_string(b) + " MB";
    }

    /* A code this helper has no words for. Printed rather than dropped: a note
       nobody can read still says something happened. */
    return "note " + std::to_string(static_cast<int>(note)) + " (" + std::to_string(a) + ", " +
           std::to_string(b) + ")";
}

} // namespace deskhop::words
