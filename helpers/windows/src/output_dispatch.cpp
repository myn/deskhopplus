#include "output_dispatch.h"

#include "words.h"

namespace deskhop {

void OutputDispatch::apply(const Output &output) {
    /* No `default:`, deliberately, in this switch and the one below. An output
       kind added to a service and forgotten here is then a compile error
       rather than a silent fall-through — /W4 /WX on MSVC and -Werror=switch
       elsewhere — and the census in the test says the same thing again. */
    switch (output.kind) {
    case Output::Kind::StoreBoardKey:
        if (!effects_.store_board_key(output.bytes))
            effects_.log("paired, but the board key could not be stored — pairing will not "
                         "survive a restart");
        break;

    case Output::Kind::OpenChannels:
        effects_.acquire_channels();
        break;

    case Output::Kind::CloseChannels:
        effects_.release_channels();
        break;

    case Output::Kind::Send:
        /* The same rule as a clipboard frame in emit(), and #107 is what it
           cost to have it in only one of the two places: the idle timer is
           charged for what the transport actually took. A beat charged for one
           it refused bought a full interval of silence, and three of those has
           the board evict this helper. Said out loud too — a refusal here used
           to be indistinguishable from a healthy quiet link. */
        if (effects_.send(output.bytes)) effects_.note_sent();
        else {
            effects_.note_send_refused();
            effects_.log("a session frame was not taken by the transport and is lost");
        }
        break;

    case Output::Kind::State:
        if (!words::state_is_known(output.state))
            effects_.log("the core reported state " +
                         std::to_string(static_cast<int>(output.state)) +
                         ", which this helper has no words for");
        effects_.log("state: " + (words::state_message(output.state).empty()
                                      ? std::string("(nothing to report)")
                                      : words::state_message(output.state)));
        effects_.show_state(output.state);
        break;

    case Output::Kind::ClipPolicy:
        emit(effects_.clip_policy_changed(output.clip_flags, output.clip_cap_mb));
        break;

    case Output::Kind::Retry:
        /* The deadline is the run loop's to hold: the clock is 32-bit
           milliseconds and wraps, so it is compared there as an unsigned
           difference rather than as `now >= then`. */
        effects_.schedule_retry(output.retry_after_ms);
        break;

    case Output::Kind::Note:
        effects_.log(output.note);
        break;
    }
}

void OutputDispatch::apply(const std::vector<Output> &outputs) {
    for (const Output &output : outputs) apply(output);
}

/*
 * The clipboard's outputs: frames to authenticate and send, payloads to write,
 * and diagnostics.
 *
 * Every frame goes out through `build_frame` — HelperSession::emit — never
 * with a counter of this layer's own, because the counter space belongs to the
 * session key and the heartbeat is already writing into it. `note_sent` is
 * what keeps ADR-0004's beat out of a direction that is far from idle.
 */
void OutputDispatch::emit(const ClipOutput &output) {
    switch (output.kind) {
    case ClipOutput::Kind::Send: {
        std::vector<uint8_t> frame;
        if (!effects_.build_frame(output.type, output.bytes, frame)) {
            effects_.log("a clipboard frame could not be built; there is no session");
            break;
        }
        /* The idle timer is charged only for a frame the transport actually
           took. Charging for one it refused would suppress a beat that
           ADR-0004 owed the board — which is exactly what HelperSession::emit
           says not to do. */
        if (effects_.send(frame)) {
            effects_.note_sent();
        } else {
            /* Counted and said out loud, as the session path above does and as
               macOS already did here. Dropped in silence, a frame the
               transport would not take is indistinguishable from one lost on
               the wire, and the two have nothing in common to fix (#132,
               #107). */
            effects_.note_send_refused();
            effects_.log("a clipboard frame of type " + std::to_string(output.type) +
                         " was not taken by the transport and is lost");
        }
        break;
    }

    case ClipOutput::Kind::Deliver:
        if (output.payload_kind == static_cast<uint8_t>(ClipKind::Text)) {
            effects_.deliver_text(output.bytes);
        } else if (output.payload_kind == static_cast<uint8_t>(ClipKind::Png)) {
            effects_.deliver_image(output.bytes);
        } else {
            effects_.log("a payload of kind " + std::to_string(output.payload_kind) +
                         " arrived, which this helper does not write");
        }
        break;

    case ClipOutput::Kind::LazyImage:
        effects_.lazy_image(output.transfer_id, output.total);
        break;

    case ClipOutput::Kind::CancelLazyImage:
        effects_.cancel_lazy_image(output.transfer_id);
        break;

    case ClipOutput::Kind::FileOffer:
        effects_.ask_about_files(
            deskhop::FileOffer{output.transfer_id, output.total, output.files});
        break;

    case ClipOutput::Kind::FileOfferWithdrawn:
        effects_.withdraw_file_question(output.transfer_id);
        break;

    case ClipOutput::Kind::DeliverFiles:
        effects_.deliver_files(FileDelivery{output.files, output.bytes});
        break;

    case ClipOutput::Kind::Note:
        effects_.log(output.note);
        break;

    case ClipOutput::Kind::ProtocolError:
        effects_.log("clipboard protocol error: " + output.note + "; dropping the connection");
        effects_.release_channels();
        break;
    }
}

void OutputDispatch::emit(const std::vector<ClipOutput> &outputs) {
    for (const ClipOutput &output : outputs) emit(output);
}

} // namespace deskhop
