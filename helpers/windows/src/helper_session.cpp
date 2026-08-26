#include "helper_session.h"

#include <cstring>
#include <stdexcept>

#include "words.h"

namespace deskhop {

namespace {

/* A vector into one of the core's fixed-size byte fields. The sizes must match
   exactly: a key handed over one byte short would fail every tag check on the
   board with nothing naming the cause, so this refuses rather than pads. */
void copy_exact(const std::vector<uint8_t> &source, uint8_t *destination, size_t size) {
    if (source.size() != size)
        throw std::invalid_argument("identity field is the wrong size");
    std::memcpy(destination, source.data(), size);
}

} // namespace

HelperSession::HelperSession(Identity identity, const std::vector<uint8_t> &board_public_key,
                             std::function<void(uint8_t *out, size_t len)> entropy)
    : callbacks_(std::make_unique<Callbacks>(Callbacks{std::move(identity), std::move(entropy), nullptr})),
      identity_(std::make_unique<dh_helper_identity>()),
      machine_(std::make_unique<dh_helper>()),
      outputs_(std::make_unique<dh_helper_outputs>()) {

    *identity_ = dh_helper_identity{};
    identity_->ctx = callbacks_.get();
    identity_->os = static_cast<uint8_t>(DH_OS_WINDOWS);
    /* This helper's own build, not the device's. The device's arrives in the
       hello_ack and the core reports it as a note. */
    identity_->build_type = static_cast<uint8_t>(DH_BUILD_RELEASE);
    copy_exact(callbacks_->identity.public_key, identity_->public_key, DH_P256_PUBLIC_SIZE);
    copy_exact(callbacks_->identity.key_id, identity_->key_id, DH_KEY_ID_SIZE);

    identity_->ecdh = [](void *ctx, const uint8_t *board_public, uint8_t *shared) -> bool {
        auto *self = static_cast<Callbacks *>(ctx);
        return self->identity.ecdh(board_public, shared);
    };
    identity_->entropy = [](void *ctx, uint8_t *out, size_t len) {
        static_cast<Callbacks *>(ctx)->entropy(out, len);
    };

    *machine_ = dh_helper{};
    *outputs_ = dh_helper_outputs{};
    dh_helper_init(machine_.get(), identity_.get(),
                   board_public_key.size() == DH_P256_PUBLIC_SIZE ? board_public_key.data()
                                                                 : nullptr);

    /* The same backing store the identity callbacks use, for the same reason:
       the core keeps a bare pointer to it for the life of the machine. */
    dh_helper_set_payload_sink(
        machine_.get(),
        [](void *ctx, uint8_t type, const uint8_t *body, size_t len) {
            auto *self = static_cast<Callbacks *>(ctx);
            if (self->payload) self->payload(type, body, len);
        },
        callbacks_.get());
}

void HelperSession::set_payload_sink(
    std::function<void(uint8_t type, const uint8_t *body, size_t len)> sink) {
    callbacks_->payload = std::move(sink);
}

bool HelperSession::emit(uint8_t type, const std::vector<uint8_t> &body,
                         std::vector<uint8_t> &out) {
    out.assign(DH_FRAME_MAX_SIZE, 0);
    size_t written = 0;
    const dh_frame_result rc = dh_helper_emit(machine_.get(), type, 0, body.data(), body.size(),
                                              out.data(), out.size(), &written);
    if (rc != DH_FRAME_OK) {
        out.clear();
        return false;
    }
    out.resize(written);
    return true;
}

void HelperSession::note_sent(uint32_t now_ms) { dh_helper_note_sent(machine_.get(), now_ms); }
void HelperSession::note_send_refused() { dh_helper_note_send_refused(machine_.get()); }

std::vector<Output> HelperSession::device_appeared(dh_device_identity which, uint32_t now_ms) {
    dh_helper_outputs_reset(outputs_.get());
    dh_helper_device_appeared(machine_.get(), which, now_ms, outputs_.get());
    return collect({});
}

std::vector<Output> HelperSession::device_disappeared(uint32_t now_ms) {
    dh_helper_outputs_reset(outputs_.get());
    dh_helper_device_disappeared(machine_.get(), now_ms, outputs_.get());
    return collect({});
}

std::vector<Output> HelperSession::channels_acquired(uint8_t count, uint32_t now_ms) {
    dh_helper_outputs_reset(outputs_.get());
    dh_helper_channels_acquired(machine_.get(), count, now_ms, outputs_.get());
    return collect({});
}

std::vector<Output> HelperSession::acquisition_refused(uint8_t acquired, uint8_t of,
                                                       uint32_t now_ms) {
    dh_helper_outputs_reset(outputs_.get());
    dh_helper_acquisition_refused(machine_.get(), acquired, of, now_ms, outputs_.get());
    return collect({});
}

std::vector<Output> HelperSession::received(const uint8_t *data, size_t len, uint32_t now_ms) {
    dh_helper_outputs_reset(outputs_.get());
    dh_helper_received(machine_.get(), data, len, now_ms, outputs_.get());
    return collect({});
}

std::vector<Output> HelperSession::transport_failed(const std::string &reason, uint32_t now_ms) {
    dh_helper_outputs_reset(outputs_.get());
    dh_helper_transport_failed(machine_.get(), now_ms, outputs_.get());
    return collect(reason);
}

std::vector<Output> HelperSession::tick(uint32_t now_ms) {
    dh_helper_outputs_reset(outputs_.get());
    dh_helper_tick(machine_.get(), now_ms, outputs_.get());
    return collect({});
}

std::vector<Output> HelperSession::collect(const std::string &transport_reason) {
    std::vector<Output> result;
    const size_t count = outputs_->count < DH_HELPER_OUTPUTS_MAX ? outputs_->count
                                                                 : DH_HELPER_OUTPUTS_MAX;

    for (size_t i = 0; i < count; ++i) {
        const dh_helper_output &item = outputs_->items[i];
        Output out;
        switch (static_cast<dh_helper_output_kind>(item.kind)) {
        case DH_HELPER_OUT_STORE_BOARD_KEY:
            out.kind = Output::Kind::StoreBoardKey;
            out.bytes.assign(item.bytes, item.bytes + item.len);
            break;
        case DH_HELPER_OUT_OPEN_CHANNELS:
            out.kind = Output::Kind::OpenChannels;
            break;
        case DH_HELPER_OUT_CLOSE_CHANNELS:
            out.kind = Output::Kind::CloseChannels;
            break;
        case DH_HELPER_OUT_SEND:
            out.kind = Output::Kind::Send;
            out.bytes.assign(item.bytes, item.bytes + item.len);
            break;
        case DH_HELPER_OUT_STATE:
            out.kind = Output::Kind::State;
            out.state = static_cast<dh_helper_state>(item.state);
            break;
        case DH_HELPER_OUT_RETRY:
            out.kind = Output::Kind::Retry;
            out.retry_after_ms = static_cast<uint32_t>(item.a);
            break;
        case DH_HELPER_OUT_NOTE:
            out.kind = Output::Kind::Note;
            out.note = words::note_line(static_cast<dh_helper_note>(item.note), item.a, item.b,
                                        transport_reason);
            break;
        case DH_HELPER_OUT_CLIP_POLICY:
            out.kind = Output::Kind::ClipPolicy;
            out.clip_flags = static_cast<uint8_t>(item.a);
            break;
        default:
            /* Unreachable while both sides come out of one build — and said
               rather than dropped, because an output that vanishes looks
               exactly like nothing having happened. */
            out.kind = Output::Kind::Note;
            out.note = "the core produced output kind " + std::to_string(item.kind) +
                       ", which this helper does not carry";
            break;
        }
        result.push_back(std::move(out));
    }

    /*
     * An output that did not fit would look exactly like nothing having
     * happened — a dropped `send`, or a state the user is never told. The core
     * counts them rather than letting them pass, and this is where that count
     * becomes visible instead of silent.
     */
    if (outputs_->overflow > 0) {
        Output lost;
        lost.kind = Output::Kind::Note;
        lost.note = std::to_string(outputs_->overflow) + " output(s) did not fit and were lost";
        result.push_back(std::move(lost));
    }

    return result;
}

} // namespace deskhop
