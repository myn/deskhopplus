import DHCore
import Foundation

/*
 * The wording of the diagnostics, which is the half of an output the shared
 * core deliberately does not carry (#80). `dh_helper` emits a code and its
 * numbers — `DH_NOTE_LISTENER_DETECTED, a = 4, b = 10000` — and each helper
 * says it in its own words; a Windows tray and a macOS log are not one string
 * table living in C.
 *
 * The rule the codes exist to protect: **a note never loses its numbers**. A
 * rate reported without the rate is what let #94 run for two days, so every
 * case below that has an `a` or a `b` spends it.
 */
enum HelperNotes {
    /// One log line for one output. `transportReason` is the platform's own
    /// description of a write that failed, which the core has no field for —
    /// it takes the failure, not the sentence.
    static func line(_ note: dh_helper_note, a: Int32, b: Int32,
                     transportReason: String?) -> String {
        switch note {
        case DH_NOTE_IGNORED_OUTSIDE_SESSION:
            return "ignoring a \(typeName(a)) that arrived outside a session"
        case DH_NOTE_IGNORED_WRONG_CORRELATION:
            return "ignoring a \(typeName(a)) with the wrong correlation value"
        case DH_NOTE_UNDECODABLE:
            return "ignoring a \(typeName(a)) that could not be decoded"
        case DH_NOTE_HELLO_ENCODE_FAILED:
            return "the hello could not be encoded (frame result \(a))"
        case DH_NOTE_ASKING_TO_BE_PAIRED:
            return "asking to be paired: the handshake is not completing"
        case DH_NOTE_PARTIAL_ACQUISITION:
            return "released \(a) of \(b) channels: a partial acquisition is not a session"
        case DH_NOTE_EVERY_CHANNEL_REFUSED:
            return "every channel refused"
        case DH_NOTE_PROTOCOL_ERROR:
            return "protocol error on the channel (frame result \(a))"
        case DH_NOTE_NO_SESSION_KEY:
            return "dropping a \(typeName(a)) with no session key"
        case DH_NOTE_TAG_FAILED:
            return "a device→helper \(typeName(a)) failed its tag"
        case DH_NOTE_COUNTER_REPLAYED:
            return "dropping a \(typeName(a)) with a counter already seen"
        case DH_NOTE_FRAME_DROPPED:
            return "dropping a \(typeName(a)) (auth result \(b))"
        case DH_NOTE_NO_BOARD_KEY:
            return "received a hello_ack but have no board key"
        case DH_NOTE_NO_STORED_NONCE:
            return "received a hello_ack with no stored nonce"
        case DH_NOTE_ACK_TOO_SHORT:
            return "a hello_ack of \(a) bytes is too short to carry a board nonce"
        case DH_NOTE_KEY_DERIVATION_FAILED:
            /* One code, two causes, and the log has to name both: the enclave
               declining and a pinned key that is not a point on the curve reach
               the core as the same ECDH returning false. */
            return "could not derive a key against the board's — the enclave refused, "
                 + "or the pinned board key is not a point on the curve"
        case DH_NOTE_DEVELOPMENT_BUILD:
            return "device is a development build: channel authentication is compiled out"
        case DH_NOTE_VERSION_MISMATCH:
            return "device speaks protocol version \(a), this helper speaks \(b)"
        case DH_NOTE_BOARD_IDENTITY_CHANGED:
            return "the board granted pairing under a different identity key — re-flashed, "
                 + "wiped past its identity sector, or swapped"
        case DH_NOTE_PAIRED_BY_DEVICE:
            return "paired by the device"
        case DH_NOTE_PAIR_REFUSED:
            switch PairRefusedReason(rawValue: UInt8(truncatingIfNeeded: a)) {
            case .noWindow: return "pairing refused: no window open"
            case .alreadyRegistered: return "pairing refused: board already has a registration"
            case nil: return "pairing refused: reason \(a)"
            }
        case DH_NOTE_FIRST_BEAT:
            return "device heartbeat: first beat of the session"
        case DH_NOTE_BEAT_RESUMED:
            return "device heartbeat resumed after \(seconds(a))"
        case DH_NOTE_BEAT_QUIET:
            return "device heartbeat quiet for \(seconds(a))"
        case DH_NOTE_SESSION_ENDED:
            /* This end's own silence goes beside the board's reason, because
               on a liveness end the two disagreeing is the finding (#107). */
            return "the device ended the session: "
                 + "\(SessionEndReason(wire: UInt8(truncatingIfNeeded: a)))"
                 /* A count, not a time since. "ms since the last send" was
                    shipped first and read 0-3 ms on every hardware sample,
                    because the helper sends in the same turn it processes the
                    session end — it described the ordering, not the link. A
                    count over the eviction window has nothing to trip on that
                    way: zero means this end agrees it was silent, and a large
                    number means the board heard none of what it sent. */
                 + "; this helper got \(b) frame(s) out over that window"
        case DH_NOTE_LISTENER_DETECTED:
            return "listener detected: \(a) refused frames in \(b)ms"
        case DH_NOTE_NO_ACK:
            return "no hello_ack within \(seconds(a))"
        case DH_NOTE_DEVICE_SILENT:
            return "nothing from the device in \(seconds(a))"
        case DH_NOTE_TRANSPORT_FAILED:
            return "transport failed: \(transportReason ?? "no reason given")"
        case DH_NOTE_RECONNECTION_RATE:
            return "the last \(a) reconnections came inside \(span(b))"
        case DH_NOTE_BOARD_SILENT_FOR:
            return "the board says it heard nothing for \(a)ms"
        case DH_NOTE_BOARD_AT_END:
            return "at the end the board had accepted \(a) frame(s) from \(b) report(s)"
        case DH_NOTE_BOARD_SENDS:
            /* Printed whole, zeros included. A zero is not proof the stream was
               intact — a frame whose tag failed never records its counter, so a
               desync reads as a complete run right up to the frame that broke
               it. DH_NOTE_STREAM_MISALIGNED is the reading for that; this one
               names whole frames lost, which is mostly the board's queue
               refusing them (its own refusal totals say how many). */
            return "the board built \(a) frame(s) for this helper; \(b) never arrived"
        case DH_NOTE_STREAM_MISALIGNED:
            return "a report from the board went missing: a frame ended with a header behind "
                 + "it where padding belongs (\(a) this session)"
        case DH_NOTE_LOCAL_SENDS:
            return "this helper has written \(a) frame(s) since boot, \(b) refused by the transport"
        case DH_NOTE_CLIP_POLICY:
            return "the board's clipboard policy: " + clipPolicy(a)
        default:
            /* A code this helper has no words for. Printed rather than dropped:
               a note nobody can read still says something happened. */
            return "note \(note.rawValue) (\(a), \(b))"
        }
    }

    /*
     * The two verbs, in words. Both are named either way round — "may send /
     * may not send" rather than listing only what is allowed — because a line
     * that says nothing about a direction reads as that direction being
     * untouched, and the whole point of this note is to say what the board
     * decided about each.
     */
    private static func clipPolicy(_ flags: Int32) -> String {
        let send = (flags & Int32(DH_CLIP_MAY_SEND)) != 0 ? "may send" : "may not send"
        let receive = (flags & Int32(DH_CLIP_MAY_RECEIVE)) != 0 ? "may receive" : "may not receive"
        return "\(send), \(receive)"
    }

    private static func seconds(_ ms: Int32) -> String {
        String(format: "%.1fs", Double(ms) / 1000)
    }

    /* The same, for a span that reaches into minutes — which the slow
       reconnection window does (#107). Three quarters of an hour printed in
       tenths of a second is the number, but not one anybody reads at a
       glance. */
    private static func span(_ ms: Int32) -> String {
        ms < 60_000 ? seconds(ms) : String(format: "%.1f min", Double(ms) / 60_000)
    }

    /* The protocol's own spelling for a frame type, so a log line and
       docs/protocol.md name the same thing. */
    private static func typeName(_ type: Int32) -> String {
        switch UInt8(truncatingIfNeeded: type) {
        case MessageType.hello: return "hello"
        case MessageType.helloAck: return "hello_ack"
        case MessageType.helloRefused: return "hello_refused"
        case MessageType.listenerAlert: return "listener_alert"
        case MessageType.heartbeat: return "heartbeat"
        case MessageType.deviceHeartbeat: return "device_heartbeat"
        case MessageType.sessionEnd: return "session_end"
        case MessageType.pairRequest: return "pair_request"
        case MessageType.pairGrant: return "pair_grant"
        case MessageType.pairRefused: return "pair_refused"
        default: return String(format: "frame of type 0x%02x", type)
        }
    }
}
