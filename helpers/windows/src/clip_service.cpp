#include "clip_service.h"

#include <cstring>

#include "dh_clip.h"
#include "dh_frame.h"
#include "dh_p256.h"
#include "dh_session.h"

namespace deskhop {

namespace {

/* Enough for the widest call. Every entry point but one is bounded by
   DH_XFER_BATCH_MAX + 2; dh_xfer_handle_done sweeps for gaps and wants one
   action per missing chunk, which it truncates safely — a truncated round of
   re-requests simply repeats at the next DONE. */
constexpr size_t kActionCapacity = 64;

/* The largest body either direction can produce: a frame's payload less the
   hop's authentication prefix. */
constexpr size_t kMaxBody = DH_FRAME_MAX_PAYLOAD - DH_FRAME_AUTH_PREFIX_SIZE;

ClipOutput note(std::string text) {
    ClipOutput out;
    out.kind = ClipOutput::Kind::Note;
    out.note = std::move(text);
    return out;
}

ClipOutput send(uint8_t type, const uint8_t *body, size_t len) {
    ClipOutput out;
    out.kind = ClipOutput::Kind::Send;
    out.type = type;
    out.bytes.assign(body, body + len);
    return out;
}

ClipOutput send(uint8_t type, const std::vector<uint8_t> &body) {
    return send(type, body.data(), body.size());
}

/* The transfer's unsealed control messages: a transfer id, sometimes a
   sequence number, and nothing else. They carry no user bytes, so sealing them
   would add sixteen bytes each to hide nothing (docs/protocol.md). */
std::vector<uint8_t> encode_id(uint32_t id) {
    uint8_t buffer[4];
    const int written = dh_clip_encode_id(id, buffer, sizeof buffer);
    return written > 0 ? std::vector<uint8_t>(buffer, buffer + written) : std::vector<uint8_t>{};
}

std::vector<uint8_t> encode_retransmit(uint32_t id, uint32_t seq) {
    uint8_t buffer[8];
    const int written = dh_clip_encode_retransmit(id, seq, buffer, sizeof buffer);
    return written > 0 ? std::vector<uint8_t>(buffer, buffer + written) : std::vector<uint8_t>{};
}

std::vector<uint8_t> encode_credit(uint32_t id, uint16_t credits) {
    uint8_t buffer[6];
    const int written = dh_clip_encode_credit(id, credits, buffer, sizeof buffer);
    return written > 0 ? std::vector<uint8_t>(buffer, buffer + written) : std::vector<uint8_t>{};
}

const char *fail_reason(uint8_t reason) {
    switch (reason) {
    case DH_XFER_FAIL_CANCELLED: return "it was cancelled";
    case DH_XFER_FAIL_LINK_DROP: return "the session went away";
    case DH_XFER_FAIL_NO_DATA: return "the payload could not be produced";
    default: return "an unrecorded reason";
    }
}

void append(std::vector<ClipOutput> &into, std::vector<ClipOutput> &&more) {
    for (ClipOutput &item : more) into.push_back(std::move(item));
}

} // namespace

ClipService::ClipService(const dh_seal_aead *aead, std::function<void(uint8_t *, size_t)> entropy,
                         size_t capacity)
    : aead_(aead), entropy_(std::move(entropy)), xfer_(std::make_unique<dh_xfer>()),
      rx_buffer_(capacity) {
    dh_seal_tx_init(&seal_tx_);
    dh_seal_rx_init(&seal_rx_);
    dh_xfer_init(xfer_.get(), rx_buffer_.data(), rx_buffer_.size());
}

void ClipService::draw(uint8_t *out, size_t len) { entropy_(out, len); }

// ------------------------------------------------------- what this computer does

std::vector<ClipOutput> ClipService::local_copy(ClipKind kind, const std::vector<uint8_t> &bytes) {
    if (!may_send_)
        return {note("a copy was not offered: the board has clipboard sending turned off in "
                     "this direction")};
    if (bytes.empty()) return {};
    if (aead_ == nullptr)
        return {note("a copy was not offered: this machine has no AES-GCM provider, and a "
                     "payload never goes out unsealed")};

    have_pending_ = true;
    pending_kind_ = static_cast<uint8_t>(kind);
    pending_ = bytes;
    return start_pending_if_sealed();
}

std::vector<ClipOutput> ClipService::policy_changed(uint8_t flags) {
    const bool could_send = may_send_;
    const bool could_receive = may_receive_;
    may_send_ = (flags & DH_CLIP_MAY_SEND) != 0;
    may_receive_ = (flags & DH_CLIP_MAY_RECEIVE) != 0;

    std::vector<ClipOutput> outputs;
    dh_xfer_action actions[kActionCapacity];

    if (could_send && !may_send_) {
        have_pending_ = false;
        pending_.clear();
        reoffer_when_sealed_ = false;
        const size_t n = dh_xfer_cancel_tx(xfer_.get(), actions, kActionCapacity);
        append(outputs, render(actions, n));
        outputs.push_back(
            note("clipboard sending was turned off; anything in flight was abandoned"));
    }
    if (could_receive && !may_receive_) {
        const size_t n = dh_xfer_cancel_rx(xfer_.get(), actions, kActionCapacity);
        append(outputs, render(actions, n));
        outputs.push_back(
            note("clipboard receiving was turned off; anything in flight was abandoned"));
    }
    return outputs;
}

std::vector<ClipOutput> ClipService::session_ended() {
    have_pending_ = false;
    pending_.clear();
    reoffer_when_sealed_ = false;

    dh_xfer_action actions[kActionCapacity];
    const size_t n = dh_xfer_link_down(xfer_.get(), actions, kActionCapacity);
    std::vector<ClipOutput> rendered = render(actions, n);
    tx_payload_.clear();

    dh_seal_tx_init(&seal_tx_);
    dh_seal_rx_init(&seal_rx_);

    /* Sends produced here have nowhere to go: there is no session to
       authenticate them. Dropped rather than handed on, so a caller cannot
       mistake them for frames that went out. */
    std::vector<ClipOutput> outputs;
    for (ClipOutput &item : rendered)
        if (item.kind != ClipOutput::Kind::Send) outputs.push_back(std::move(item));
    return outputs;
}

std::vector<ClipOutput> ClipService::pump() {
    dh_xfer_action actions[kActionCapacity];
    const size_t n = dh_xfer_pump(xfer_.get(), actions, kActionCapacity);
    return render(actions, n);
}

std::string ClipService::progress_line() const {
    std::string out;
    if (dh_xfer_is_sending(xfer_.get())) {
        out += "sending " + std::to_string(dh_xfer_tx_next_seq(xfer_.get())) + "/" +
               std::to_string(dh_xfer_tx_chunks(xfer_.get())) + " chunks";
        if (!dh_xfer_tx_streaming(xfer_.get())) out += ", never requested";
    }
    if (dh_xfer_is_receiving(xfer_.get())) {
        if (!out.empty()) out += ", ";
        out += "receiving " + std::to_string(dh_xfer_rx_received(xfer_.get())) + "/" +
               std::to_string(dh_xfer_rx_chunks(xfer_.get())) + " chunks";
    }
    return out.empty() ? std::string("nothing in flight") : out;
}

std::string ClipService::drops_line(const dh_device_drops *drops) {
    if (drops == nullptr) return "the board has stated no drop totals";

    const struct {
        const char *name;
        uint32_t count;
    } seams[] = {
        {"reports not taken", drops->reports},
        {"from peer board", drops->inbound},
        {"outbound refused", drops->outq},
        {"inter-board refused", drops->link},
        {"peer orphan packets", drops->orphans},
        {"peer frames truncated", drops->truncated},
        {"relay queue refused", drops->relay_q},
    };

    std::string out;
    for (const auto &seam : seams) {
        if (seam.count == 0) continue;
        if (!out.empty()) out += ", ";
        out += std::string(seam.name) + " " + std::to_string(seam.count);
    }
    return out.empty() ? std::string("board reports no drops") : "board drops: " + out;
}

std::vector<ClipOutput> ClipService::tick(uint32_t now_ms, const dh_device_drops *drops) {
    std::vector<ClipOutput> outputs;
    dh_xfer_action actions[kActionCapacity];
    const std::string board = drops_line(drops);

    /* Unsigned differences throughout, so a wrapping millisecond counter is
       arithmetic rather than a transfer abandoned once every 49 days. */
    if (!dh_xfer_is_sending(xfer_.get())) {
        sending_timed_ = false;
    } else if (!sending_timed_ || sending_mark_ != tx_progress_) {
        sending_timed_ = true;
        sending_since_ = now_ms;
        sending_mark_ = tx_progress_;
    } else if (now_ms - sending_since_ >= kStallTimeoutMs) {
        const std::string line = progress_line();
        sending_timed_ = false;
        have_pending_ = false;
        pending_.clear();
        reoffer_when_sealed_ = false;
        append(outputs, render(actions, dh_xfer_cancel_tx(xfer_.get(), actions, kActionCapacity)));
        outputs.push_back(note("a transfer made no progress for " +
                               std::to_string(kStallTimeoutMs / 1000) + "s and was abandoned (" +
                               line + "; " + board +
                               "); the other computer's helper is not answering"));
    }

    if (!dh_xfer_is_receiving(xfer_.get())) {
        receiving_timed_ = false;
    } else if (!receiving_timed_ || receiving_mark_ != rx_progress_) {
        receiving_timed_ = true;
        receiving_since_ = now_ms;
        receiving_mark_ = rx_progress_;
    } else if (now_ms - receiving_since_ >= kStallTimeoutMs) {
        const std::string line = progress_line();
        receiving_timed_ = false;
        append(outputs, render(actions, dh_xfer_cancel_rx(xfer_.get(), actions, kActionCapacity)));
        outputs.push_back(note("an arriving transfer made no progress for " +
                               std::to_string(kStallTimeoutMs / 1000) + "s and was abandoned (" +
                               line + "; " + board + "); nothing partial is ever written"));
    }
    return outputs;
}

// -------------------------------------------------------- what the far helper says

std::vector<ClipOutput> ClipService::received(uint8_t type, const uint8_t *body, size_t len) {
    dh_xfer_action actions[kActionCapacity];
    uint32_t id = 0;
    uint32_t seq = 0;
    uint16_t credits = 0;
    std::vector<ClipOutput> outputs;

    switch (type) {
    case DH_MSG_SEAL_OFFER:
        return on_seal_offered(body, len);
    case DH_MSG_SEAL_ACCEPT:
        return on_seal_accepted(body, len);
    case DH_MSG_SEAL_STALE:
        return on_seal_stale(body, len);
    case DH_MSG_CLIP_OFFER:
        return on_offer(body, len);
    case DH_MSG_CLIP_CHUNK:
        return on_chunk(body, len);

    case DH_MSG_CLIP_REQUEST:
        if (!dh_clip_decode_id(body, len, &id)) break;
        outputs = render(actions, dh_xfer_handle_request(xfer_.get(), id, actions,
                                                         kActionCapacity));
        append(outputs, pump());
        return outputs;

    case DH_MSG_CLIP_DONE:
        if (!dh_clip_decode_id(body, len, &id)) break;
        return render(actions, dh_xfer_handle_done(xfer_.get(), id, actions, kActionCapacity));

    case DH_MSG_CLIP_CANCEL:
        if (!dh_clip_decode_id(body, len, &id)) break;
        return render(actions, dh_xfer_handle_cancel(xfer_.get(), id, actions, kActionCapacity));

    case DH_MSG_CLIP_RETRANSMIT:
        if (!dh_clip_decode_retransmit(body, len, &id, &seq)) break;
        outputs = render(actions, dh_xfer_handle_retransmit(xfer_.get(), id, seq, actions,
                                                            kActionCapacity));
        append(outputs, pump());
        return outputs;

    case DH_MSG_CLIP_CREDIT:
        if (!dh_clip_decode_credit(body, len, &id, &credits)) break;
        outputs = render(actions, dh_xfer_handle_credit(xfer_.get(), id, credits, actions,
                                                        kActionCapacity));
        append(outputs, pump());
        return outputs;

    default:
        return {note("a clipboard message of type " + std::to_string(type) +
                     " arrived, which this helper does not carry")};
    }

    /* Only the decode failures reach here. A control message carries transfer
       ids and sequence numbers, so acting on one that would not decode
       cancels or re-requests the wrong thing. */
    return {note("a clipboard message of type " + std::to_string(type) + " would not decode")};
}

// ------------------------------------------------------------------ the seal exchange

std::vector<ClipOutput> ClipService::on_seal_offered(const uint8_t *body, size_t len) {
    uint8_t reply[DH_SEAL_EXCHANGE_LEN];
    uint8_t nonce[DH_NONCE_SIZE];
    draw(nonce, sizeof nonce);

    /*
     * Random 32 bytes are a usable P-256 scalar all but about once in 2^32
     * draws — rare enough to be a retry and far too common to be a crash. A
     * handful of attempts is already beyond any plausible run of bad luck;
     * past that, the entropy source is what is wrong.
     */
    for (int attempt = 0; attempt < 8; attempt++) {
        uint8_t eph_private[DH_P256_PRIVATE_SIZE];
        draw(eph_private, sizeof eph_private);
        size_t written = 0;
        const dh_seal_result rc = dh_seal_rx_offered(&seal_rx_, body, len, eph_private, nonce,
                                                     reply, sizeof reply, &written);
        if (rc == DH_SEAL_OK) return {send(DH_MSG_SEAL_ACCEPT, reply, written)};
        if (rc != DH_SEAL_ERR_KEY)
            return {note("a seal offer could not be accepted: error " + std::to_string(rc))};
    }
    return {note("a seal offer could not be accepted: no usable ephemeral key was drawn")};
}

std::vector<ClipOutput> ClipService::on_seal_accepted(const uint8_t *body, size_t len) {
    const dh_seal_result rc = dh_seal_tx_accepted(&seal_tx_, body, len);
    if (rc != DH_SEAL_OK)
        return {note("a seal accept could not be used: error " + std::to_string(rc))};

    /* The copy that was waiting for exactly this, or the transfer a stale seal
       knocked back to the start. */
    if (reoffer_when_sealed_) {
        reoffer_when_sealed_ = false;
        if (!have_pending_) return reoffer();
    }
    return start_pending_if_sealed();
}

std::vector<ClipOutput> ClipService::on_seal_stale(const uint8_t *body, size_t len) {
    uint32_t seal_id = 0;
    if (!dh_seal_decode_stale(body, len, &seal_id))
        return {note("a SEAL_STALE would not decode")};

    /* A stale naming some other seal changes nothing: this end has already
       moved on, and re-offering would restart a transfer that is working. */
    if (!dh_seal_tx_stale(&seal_tx_, seal_id)) return {};

    if (!have_pending_) {
        dh_clip_offer current{};
        if (!dh_xfer_offer_info(xfer_.get(), &current))
            return {note("the far helper lost the seal; nothing was waiting on it")};
        reoffer_when_sealed_ = true;
    }

    std::vector<ClipOutput> outputs = offer_seal();
    outputs.push_back(note("the far helper lost the seal; offering a fresh one"));
    return outputs;
}

// -------------------------------------------------------------- receiving a payload

std::vector<ClipOutput> ClipService::on_offer(const uint8_t *body, size_t len) {
    /*
     * Refused before the seal is opened, deliberately. A helper told not to
     * receive has no business decrypting the payload first — and the clear head
     * carries everything a refusal needs.
     */
    if (!may_receive_) {
        dh_clip_offer_head head{};
        if (!dh_clip_decode_offer_head(body, len, &head))
            return {note("an offer would not decode")};
        std::vector<ClipOutput> outputs;
        outputs.push_back(send(DH_MSG_CLIP_CANCEL, encode_id(head.id)));
        outputs.push_back(note("an offer was refused: the board has clipboard receiving turned "
                               "off in this direction"));
        return outputs;
    }

    std::vector<uint8_t> plain(kMaxBody);
    dh_clip_offer offer{};
    const dh_seal_result rc = dh_seal_open_offer(&seal_rx_, aead_, body, len, plain.data(),
                                                 plain.size(), &offer);
    if (rc == DH_SEAL_ERR_UNKNOWN_ID) return stale_reply(DH_MSG_CLIP_OFFER, body, len);
    if (rc != DH_SEAL_OK)
        return {note("an offer could not be opened: error " + std::to_string(rc))};

    dh_xfer_action actions[kActionCapacity];
    return render(actions,
                  dh_xfer_handle_offer(xfer_.get(), &offer, actions, kActionCapacity));
}

std::vector<ClipOutput> ClipService::on_chunk(const uint8_t *body, size_t len) {
    /*
     * Refused before the seal is opened, like the offer — the invariant is that
     * a helper told not to receive never decrypts a payload it has already
     * decided to refuse (docs/protocol.md).
     *
     * This is reachable in the ordinary way: turning the toggle off
     * mid-transfer cancels the transfer, but the chunks already in flight
     * behind that cancel keep arriving. Silent because the cancel already said
     * why, and there are up to a credit window of these — a line each would
     * bury the reason under its own consequences.
     */
    if (!may_receive_) return {};

    std::vector<uint8_t> plain(kMaxBody);
    dh_clip_chunk chunk{};
    const dh_seal_result rc = dh_seal_open_chunk(&seal_rx_, aead_, body, len, plain.data(),
                                                 plain.size(), &chunk);
    if (rc == DH_SEAL_ERR_UNKNOWN_ID) return stale_reply(DH_MSG_CLIP_CHUNK, body, len);
    if (rc != DH_SEAL_OK)
        return {note("a chunk could not be opened: error " + std::to_string(rc))};

    /*
     * Before and after, because the transfer machine refuses a chunk by doing
     * nothing: one for the wrong transfer, out of range, or whose CRC32 does
     * not match is dropped with no action. Without this the difference between
     * "no chunk arrived" and "every chunk arrived and was refused" is
     * invisible, and those two have nothing in common to fix.
     */
    const uint32_t before = dh_xfer_rx_received(xfer_.get());
    dh_xfer_action actions[kActionCapacity];
    std::vector<ClipOutput> outputs =
        render(actions, dh_xfer_handle_chunk(xfer_.get(), &chunk, actions, kActionCapacity));
    if (dh_xfer_rx_received(xfer_.get()) == before)
        outputs.push_back(note("chunk " + std::to_string(chunk.seq) + " of transfer " +
                               std::to_string(chunk.id) +
                               " opened but the transfer machine refused it (" + progress_line() +
                               ")"));
    return outputs;
}

std::vector<ClipOutput> ClipService::stale_reply(uint8_t type, const uint8_t *body, size_t len) {
    uint32_t seal_id = 0;
    if (!dh_seal_peek_id(type, body, len, &seal_id))
        return {note("a sealed message would not decode")};

    uint8_t out[DH_SEAL_STALE_LEN];
    const int written = dh_seal_encode_stale(seal_id, out, sizeof out);
    if (written <= 0) return {note("a SEAL_STALE would not encode")};
    return {send(DH_MSG_SEAL_STALE, out, static_cast<size_t>(written))};
}

// ---------------------------------------------------------------- sending a payload

std::vector<ClipOutput> ClipService::start_pending_if_sealed() {
    if (!have_pending_) return {};
    if (!seal_tx_.live) return offer_seal();

    have_pending_ = false;
    tx_payload_ = std::move(pending_);
    pending_.clear();

    dh_xfer_action actions[kActionCapacity];
    const size_t n = dh_xfer_offer(xfer_.get(), pending_kind_, nullptr, 0, tx_payload_.data(),
                                   tx_payload_.size(), actions, kActionCapacity);
    return render(actions, n);
}

std::vector<ClipOutput> ClipService::reoffer() {
    dh_clip_offer current{};
    if (tx_payload_.empty() || !dh_xfer_offer_info(xfer_.get(), &current)) return {};

    dh_xfer_action actions[kActionCapacity];
    const size_t n = dh_xfer_offer(xfer_.get(), current.kind, nullptr, 0, tx_payload_.data(),
                                   tx_payload_.size(), actions, kActionCapacity);
    return render(actions, n);
}

std::vector<ClipOutput> ClipService::offer_seal() {
    if (aead_ == nullptr) {
        have_pending_ = false;
        pending_.clear();
        return {note("no AES-GCM provider, so nothing can be sealed and nothing can be sent")};
    }

    uint8_t seal_id_bytes[DH_SEAL_ID_SIZE];
    draw(seal_id_bytes, sizeof seal_id_bytes);
    const uint32_t seal_id = static_cast<uint32_t>(seal_id_bytes[0]) |
                             (static_cast<uint32_t>(seal_id_bytes[1]) << 8) |
                             (static_cast<uint32_t>(seal_id_bytes[2]) << 16) |
                             (static_cast<uint32_t>(seal_id_bytes[3]) << 24);

    uint8_t nonce[DH_NONCE_SIZE];
    draw(nonce, sizeof nonce);

    uint8_t out[DH_SEAL_EXCHANGE_LEN];
    for (int attempt = 0; attempt < 8; attempt++) {
        uint8_t eph_private[DH_P256_PRIVATE_SIZE];
        draw(eph_private, sizeof eph_private);
        size_t written = 0;
        const dh_seal_result rc = dh_seal_tx_offer(&seal_tx_, seal_id, eph_private, nonce, out,
                                                   sizeof out, &written);
        if (rc == DH_SEAL_OK) return {send(DH_MSG_SEAL_OFFER, out, written)};
        if (rc != DH_SEAL_ERR_KEY) break;
    }

    have_pending_ = false;
    pending_.clear();
    return {note("a seal could not be offered, so nothing can be sent")};
}

// ------------------------------------------------------------- actions into messages

std::vector<ClipOutput> ClipService::render(const dh_xfer_action *actions, size_t count) {
    std::vector<ClipOutput> outputs;
    for (size_t i = 0; i < count; i++) {
        const dh_xfer_action &action = actions[i];
        switch (action.type) {
        case DH_XFER_ACT_SEND_OFFER:
            tx_progress_++;
            append(outputs, sealed_offer());
            break;
        case DH_XFER_ACT_SEND_CHUNK:
            tx_progress_++;
            append(outputs, sealed_chunk(action.seq));
            break;
        case DH_XFER_ACT_SEND_DONE:
            tx_progress_++;
            outputs.push_back(send(DH_MSG_CLIP_DONE, encode_id(action.id)));
            break;
        case DH_XFER_ACT_SEND_REQUEST:
            rx_progress_++;
            outputs.push_back(send(DH_MSG_CLIP_REQUEST, encode_id(action.id)));
            break;
        case DH_XFER_ACT_SEND_RETRANSMIT:
            rx_progress_++;
            outputs.push_back(
                send(DH_MSG_CLIP_RETRANSMIT, encode_retransmit(action.id, action.seq)));
            break;
        case DH_XFER_ACT_SEND_CREDIT:
            rx_progress_++;
            outputs.push_back(send(DH_MSG_CLIP_CREDIT, encode_credit(action.id, action.credits)));
            break;
        case DH_XFER_ACT_SEND_CANCEL:
            outputs.push_back(send(DH_MSG_CLIP_CANCEL, encode_id(action.id)));
            break;
        case DH_XFER_ACT_DELIVERED: {
            ClipOutput out;
            out.kind = ClipOutput::Kind::Deliver;
            out.payload_kind = dh_xfer_delivered_kind(xfer_.get());
            const uint64_t length = dh_xfer_delivered_len(xfer_.get());
            const size_t bounded = static_cast<size_t>(
                length < rx_buffer_.size() ? length : rx_buffer_.size());
            out.bytes.assign(rx_buffer_.begin(),
                             rx_buffer_.begin() + static_cast<ptrdiff_t>(bounded));
            outputs.push_back(std::move(out));
            break;
        }
        case DH_XFER_ACT_FAILED:
            outputs.push_back(note("transfer " + std::to_string(action.id) + " was abandoned: " +
                                   fail_reason(action.reason)));
            break;
        case DH_XFER_ACT_NEED_DATA: {
            /* Nothing here offers lazily, so this is the core asking for a
               payload that was never promised. Refused rather than left as a
               transfer that never finishes. */
            outputs.push_back(
                note("a lazy payload was asked for, which this slice never offers"));
            dh_xfer_action more[kActionCapacity];
            append(outputs,
                   render(more, dh_xfer_provide_fail(xfer_.get(), more, kActionCapacity)));
            break;
        }
        default:
            outputs.push_back(note("the transfer core produced action " +
                                   std::to_string(action.type) +
                                   ", which this helper does not carry"));
            break;
        }
    }
    return outputs;
}

std::vector<ClipOutput> ClipService::sealed_offer() {
    dh_clip_offer offer{};
    if (!dh_xfer_offer_info(xfer_.get(), &offer))
        return {note("an offer was asked for with no transfer in flight")};

    std::vector<uint8_t> out(kMaxBody);
    size_t written = 0;
    const dh_seal_result rc =
        dh_seal_encode_offer(&seal_tx_, aead_, &offer, out.data(), out.size(), &written);
    if (rc != DH_SEAL_OK)
        return {note("an offer could not be sealed: error " + std::to_string(rc))};
    return {send(DH_MSG_CLIP_OFFER, out.data(), written)};
}

std::vector<ClipOutput> ClipService::sealed_chunk(uint32_t seq) {
    dh_clip_chunk chunk{};
    if (!dh_xfer_chunk_at(xfer_.get(), seq, &chunk))
        return {note("chunk " + std::to_string(seq) + " was asked for and there is no such chunk")};

    std::vector<uint8_t> out(kMaxBody);
    size_t written = 0;
    const dh_seal_result rc =
        dh_seal_encode_chunk(&seal_tx_, aead_, &chunk, out.data(), out.size(), &written);
    if (rc != DH_SEAL_OK)
        return {note("chunk " + std::to_string(seq) + " could not be sealed: error " +
                     std::to_string(rc))};
    return {send(DH_MSG_CLIP_CHUNK, out.data(), written)};
}

} // namespace deskhop
