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
    case DH_XFER_FAIL_SEAL_REPLACED: return "the far helper started a fresh seal";
    default: return "an unrecorded reason";
    }
}

void append(std::vector<ClipOutput> &into, std::vector<ClipOutput> &&more) {
    for (ClipOutput &item : more) into.push_back(std::move(item));
}

/* The kind-2 offer's metadata, both ways. `dh_file_list` is the codec — this
   only moves between it and the vectors this file works in. Twin:
   FileList.swift. */
bool encode_file_list(const std::vector<FileEntry> &files, std::vector<uint8_t> &out) {
    if (files.empty() || files.size() > DH_FILE_LIST_MAX) return false;
    std::vector<dh_file_entry> raw;
    raw.reserve(files.size());
    for (const FileEntry &file : files) {
        if (file.name.size() > 0xFFFFu) return false;
        raw.push_back(dh_file_entry{file.name.data(),
                                    static_cast<uint16_t>(file.name.size()), file.size});
    }
    std::vector<char> buffer(dh_file_list_encode_max());
    const int written = dh_file_list_encode(raw.data(), static_cast<uint16_t>(raw.size()),
                                            buffer.data(), buffer.size());
    if (written <= 0) return false;
    out.assign(buffer.begin(), buffer.begin() + written);
    return true;
}

/* `total` comes back rather than being re-summed by the caller: the core has
   already added these up once, with the overflow check, and a second sum
   somewhere else is a second answer waiting to disagree. */
bool decode_file_list(const uint8_t *meta, size_t len, std::vector<FileEntry> &out,
                      uint64_t &total) {
    dh_file_list list{};
    if (!dh_file_list_decode(reinterpret_cast<const char *>(meta), len, &list)) return false;
    out.clear();
    out.reserve(list.count);
    for (uint16_t i = 0; i < list.count; i++)
        out.push_back(FileEntry{std::string(list.entries[i].name, list.entries[i].name_len),
                                list.entries[i].size});
    total = list.total;
    return true;
}

} // namespace

uint32_t FileOffer::estimated_seconds() const {
    const uint64_t rate = ClipService::kMeasuredBytesPerSecond;
    return static_cast<uint32_t>((total + rate - 1) / rate);
}

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
    pending_is_files_ = false;
    pending_kind_ = static_cast<uint8_t>(kind);
    pending_ = bytes;
    pending_provider_ = nullptr;
    seal_waiting_timed_ = false;
    return start_pending_if_sealed();
}

std::vector<ClipOutput> ClipService::local_copy_files(
    const std::vector<FileEntry> &files, std::function<bool(std::vector<uint8_t> &)> provider) {
    if (!may_send_)
        return {note("a file copy was not offered: the board has clipboard sending turned off "
                     "in this direction")};
    if (files.empty()) return {};
    if (aead_ == nullptr)
        return {note("a file copy was not offered: this machine has no AES-GCM provider, and a "
                     "payload never goes out unsealed")};

    std::vector<uint8_t> meta;
    if (!encode_file_list(files, meta))
        return {note(std::to_string(files.size()) +
                     " file(s) were copied and their names would not fit one offer, so nothing "
                     "was sent")};

    uint64_t total = 0;
    for (const FileEntry &file : files) {
        if (total > UINT64_MAX - file.size)
            return {note("the copied files add up to more than can be transferred")};
        total += file.size;
    }

    have_pending_ = true;
    pending_is_files_ = true;
    pending_kind_ = static_cast<uint8_t>(ClipKind::Files);
    pending_.clear();
    pending_files_ = files;
    pending_meta_ = std::move(meta);
    pending_total_ = total;
    pending_provider_ = std::move(provider);
    seal_waiting_timed_ = false;
    return start_pending_if_sealed();
}

std::vector<ClipOutput> ClipService::accept_files(uint32_t id) {
    if (!have_held_offer_ || held_offer_.id != id) return {};
    /*
     * The machine has to still be holding it. Without this the file list is
     * remembered for a transfer that will never run — and the *next* transfer
     * then arrives and is split by the wrong list, which reads at the desk as
     * a paste that silently never happens (#56).
     */
    if (!dh_xfer_rx_is_held(xfer_.get())) {
        have_held_offer_ = false;
        held_timed_ = false;
        ClipOutput withdrawn;
        withdrawn.kind = ClipOutput::Kind::FileOfferWithdrawn;
        withdrawn.transfer_id = id;
        std::vector<ClipOutput> outputs;
        outputs.push_back(std::move(withdrawn));
        outputs.push_back(note("the files were accepted here, but that transfer is no longer "
                               "waiting to be asked for; nothing was requested"));
        return outputs;
    }
    const deskhop::FileOffer offer = held_offer_;
    have_held_offer_ = false;
    held_timed_ = false;
    incoming_files_ = offer.files;
    have_incoming_files_ = true;

    dh_xfer_action actions[kActionCapacity];
    std::vector<ClipOutput> outputs =
        render(actions, dh_xfer_request_lazy(xfer_.get(), id, actions, kActionCapacity));
    outputs.push_back(note(std::to_string(offer.files.size()) + " file(s), " +
                           std::to_string(offer.total) +
                           " bytes, were accepted here and asked for"));
    return outputs;
}

std::vector<ClipOutput> ClipService::decline_files(uint32_t id) {
    if (!have_held_offer_ || held_offer_.id != id) return {};
    const size_t count = held_offer_.files.size();
    have_held_offer_ = false;
    held_timed_ = false;

    dh_xfer_action actions[kActionCapacity];
    std::vector<ClipOutput> outputs =
        render(actions, dh_xfer_cancel_rx(xfer_.get(), actions, kActionCapacity));
    ClipOutput withdrawn;
    withdrawn.kind = ClipOutput::Kind::FileOfferWithdrawn;
    withdrawn.transfer_id = id;
    outputs.push_back(std::move(withdrawn));
    outputs.push_back(note(std::to_string(count) +
                           " file(s) offered from the other computer were declined here"));
    return outputs;
}

std::vector<ClipOutput> ClipService::abort_receive() {
    if (have_held_offer_) return decline_files(held_offer_.id);
    if (!dh_xfer_is_receiving(xfer_.get())) return {};
    dh_xfer_action actions[kActionCapacity];
    std::vector<ClipOutput> outputs =
        render(actions, dh_xfer_cancel_rx(xfer_.get(), actions, kActionCapacity));
    outputs.push_back(
        note("an arriving transfer was cancelled here; nothing partial is kept"));
    return outputs;
}

std::vector<ClipOutput> ClipService::abort_send() {
    have_pending_ = false;
    pending_.clear();
    pending_provider_ = nullptr;
    outgoing_provider_ = nullptr;
    if (!dh_xfer_is_sending(xfer_.get())) return {};
    dh_xfer_action actions[kActionCapacity];
    std::vector<ClipOutput> outputs =
        render(actions, dh_xfer_cancel_tx(xfer_.get(), actions, kActionCapacity));
    outputs.push_back(note("a transfer leaving this computer was cancelled here"));
    return outputs;
}

std::vector<ClipOutput> ClipService::capacity_changed(uint8_t megabytes) {
    const size_t bytes = static_cast<size_t>(dh_clip_cap_bytes(megabytes));
    if (bytes == rx_buffer_.size()) {
        wanted_capacity_ = 0;
        return {};
    }
    /* Asked before the allocation: a tick retrying a refused swap would
       otherwise allocate and free up to 64 MB several times a second for the
       whole of the transfer it is waiting on. The old buffer is released only
       once the core has taken the new one, because until then it is still what
       an arriving payload is assembling into. */
    if (dh_xfer_can_set_rx_buffer(xfer_.get())) {
        std::vector<uint8_t> replacement(bytes);
        if (dh_xfer_set_rx_buffer(xfer_.get(), replacement.data(), replacement.size())) {
            rx_buffer_ = std::move(replacement);
            wanted_capacity_ = 0;
            return {note("the clipboard size cap is now " + std::to_string(megabytes) + " MB")};
        }
    }
    wanted_capacity_ = bytes;
    return {note("the clipboard size cap changed to " + std::to_string(megabytes) +
                 " MB and will take effect when nothing is arriving")};
}

bool ClipService::arriving(uint8_t *kind, uint64_t *received, uint64_t *total) const {
    if (!dh_xfer_is_receiving(xfer_.get())) return false;
    const uint64_t promised = dh_xfer_delivered_len(xfer_.get());
    const uint64_t assembled =
        static_cast<uint64_t>(dh_xfer_rx_received(xfer_.get())) * DH_XFER_CHUNK_SIZE;
    if (kind != nullptr) *kind = dh_xfer_delivered_kind(xfer_.get());
    if (received != nullptr) *received = assembled < promised ? assembled : promised;
    if (total != nullptr) *total = promised;
    return true;
}

const deskhop::FileOffer *ClipService::awaiting_decision() const {
    return have_held_offer_ ? &held_offer_ : nullptr;
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
        pending_provider_ = nullptr;
        outgoing_provider_ = nullptr;
        seal_waiting_timed_ = false;
        reoffer_when_sealed_ = false;
        const size_t n = dh_xfer_cancel_tx(xfer_.get(), actions, kActionCapacity);
        append(outputs, render(actions, n));
        outputs.push_back(
            note("clipboard sending was turned off; anything in flight was abandoned"));
    }
    if (could_receive && !may_receive_) {
        if (have_held_offer_) {
            have_held_offer_ = false;
            held_timed_ = false;
            ClipOutput withdrawn;
            withdrawn.kind = ClipOutput::Kind::FileOfferWithdrawn;
            withdrawn.transfer_id = held_offer_.id;
            outputs.push_back(std::move(withdrawn));
        }
        have_incoming_files_ = false;
        incoming_files_.clear();
        const size_t n = dh_xfer_cancel_rx(xfer_.get(), actions, kActionCapacity);
        append(outputs, render(actions, n));
        outputs.push_back(
            note("clipboard receiving was turned off; anything in flight was abandoned"));
    }
    return outputs;
}

std::vector<ClipOutput> ClipService::session_ended() {
    outgoing_provider_ = nullptr;
    have_incoming_files_ = false;
    incoming_files_.clear();
    reoffer_when_sealed_ = false;

    dh_xfer_action actions[kActionCapacity];
    std::vector<ClipOutput> rendered;
    /*
     * A copy still waiting for a seal is *kept*. What is on the clipboard does
     * not change because the link wobbled, and the clipboard is only read again
     * when the user copies something else — so dropping it here lost the copy
     * for good, in silence. `seal_waiting_since_` is deliberately left running:
     * the 30s budget is counted from the copy, across as many session ends as
     * the link manages, and `tick` still gives up out loud at the end of it.
     */
    if (have_pending_) {
        rendered.push_back(note("the session went away; " + describe_pending() +
                                " copied here are still waiting for one that can carry them"));
    }
    if (have_held_offer_) {
        have_held_offer_ = false;
        held_timed_ = false;
        ClipOutput withdrawn;
        withdrawn.kind = ClipOutput::Kind::FileOfferWithdrawn;
        withdrawn.transfer_id = held_offer_.id;
        rendered.push_back(std::move(withdrawn));
    }
    const size_t n = dh_xfer_link_down(xfer_.get(), actions, kActionCapacity);
    append(rendered, render(actions, n));
    tx_payload_.clear();
    tx_meta_.clear();

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

std::vector<ClipOutput> ClipService::request_lazy_image(uint32_t id) {
    dh_xfer_action actions[kActionCapacity];
    return render(actions, dh_xfer_request_lazy(xfer_.get(), id, actions, kActionCapacity));
}

std::vector<ClipOutput> ClipService::lazy_image_was_replaced(uint32_t id) {
    if (lazy_image_id_ != id) return {};
    dh_xfer_action actions[kActionCapacity];
    return render(actions, dh_xfer_cancel_rx(xfer_.get(), actions, kActionCapacity));
}

/* The re-request pair is on both directions and always printed, zeros
   included: a stall where nothing was ever asked for again is a different
   fault from one where it was asked for and nothing came back, and neither end
   said which before #145. */
std::string ClipService::progress_line() const {
    std::string out;
    if (dh_xfer_is_sending(xfer_.get())) {
        out += "sending " + std::to_string(dh_xfer_tx_next_seq(xfer_.get())) + "/" +
               std::to_string(dh_xfer_tx_chunks(xfer_.get())) + " chunks";
        if (!dh_xfer_tx_streaming(xfer_.get())) out += ", never requested";
        out += ", produced " + std::to_string(dh_xfer_tx_offer_retries(xfer_.get())) +
               " offer retries";
        out += ", asked for " + std::to_string(dh_xfer_tx_retx_asked(xfer_.get())) +
               " again and sent " + std::to_string(dh_xfer_tx_retx_sent(xfer_.get()));
    }
    if (dh_xfer_is_receiving(xfer_.get())) {
        if (!out.empty()) out += ", ";
        out += "receiving " + std::to_string(dh_xfer_rx_received(xfer_.get())) + "/" +
               std::to_string(dh_xfer_rx_chunks(xfer_.get())) + " chunks, asked for " +
               std::to_string(dh_xfer_rx_retx_asked(xfer_.get())) + " again and got " +
               std::to_string(dh_xfer_rx_retx_answered(xfer_.get())) + " back, observed " +
               std::to_string(dh_xfer_rx_duplicate_offers(xfer_.get())) +
               " duplicate offers";
    }
    return out.empty() ? std::string("nothing in flight") : out;
}

std::vector<ClipOutput> ClipService::sweep() {
    dh_xfer_action actions[kActionCapacity];
    const size_t n = dh_xfer_sweep_rx(xfer_.get(), actions, kActionCapacity);
    bool restarted = false;
    size_t named = 0;
    for (size_t i = 0; i < n; i++) {
        if (actions[i].type == DH_XFER_ACT_SEND_REQUEST) restarted = true;
        if (actions[i].type == DH_XFER_ACT_SEND_RETRANSMIT) named++;
    }
    std::vector<ClipOutput> outputs = render(actions, n);
    outputs.push_back(note("an arriving transfer made no progress for " +
                           std::to_string(kSweepDelayMs / 1000) + "s; " +
                           (restarted ? std::string("nothing has arrived at all, so it was asked "
                                                    "for again")
                                      : std::to_string(named) + " chunk(s) asked for again") +
                           " (" + progress_line() + ")"));
    return outputs;
}

std::string ClipService::drops_line(const dh_device_drops *drops) {
    if (drops == nullptr) return "the board has stated no drop totals";

    /*
     * The outbound total carries its breakdown, because which band refused
     * decides what to do about it and the total says only that something did
     * (#142). The queue has two bands and #141 deepened one of them.
     *
     * The bulk figure is derived: the board sends the total and the two parts
     * that are not it, because appending two fields rather than three keeps
     * the frame inside the board's reply buffer. Printed whole, zeros
     * included — a zero here is the finding, not an absence.
     */
    const uint32_t named = drops->outq_priority + drops->outq_bad_header;
    const uint32_t outq_bulk = drops->outq >= named ? drops->outq - named : 0;
    const std::string outq_detail =
        "outbound refused " + std::to_string(drops->outq) + " (priority " +
        std::to_string(drops->outq_priority) + ", bulk " + std::to_string(outq_bulk) +
        ", bad header " + std::to_string(drops->outq_bad_header) + ")";

    const struct {
        const char *name;
        uint32_t count;
        const std::string *detail; /* printed instead of "name count" when set */
    } seams[] = {
        {"reports not taken", drops->reports, nullptr},
        {"from peer board", drops->inbound, nullptr},
        {"outbound refused", drops->outq, &outq_detail},
        {"not handed on", drops->unsent, nullptr},
        {"peer orphan packets", drops->orphans, nullptr},
        {"peer frames truncated", drops->truncated, nullptr},
        {"relay queue refused", drops->relay_q, nullptr},
    };

    std::string out;
    for (const auto &seam : seams) {
        if (seam.count == 0) continue;
        if (!out.empty()) out += ", ";
        out += seam.detail != nullptr ? *seam.detail
                                      : std::string(seam.name) + " " + std::to_string(seam.count);
    }
    /* The accepted-frame count rides along whatever the losses say, and is
       never one of them: on a liveness eviction it is the number that decides
       whether the board was hearing this helper at all (#107). */
    /* The inbound chain, head to tail, so a loss can be located in one reading
       instead of one per rebuild (#107). */
    const std::string heard =
        "; board inbound: " + std::to_string(drops->reports_in) + " report(s) in, " +
        std::to_string(drops->frames_in) + " frame(s) accepted, " +
        std::to_string(drops->frames_refused) + " refused";
    return (out.empty() ? std::string("board reports no drops") : "board drops: " + out) + heard;
}

std::vector<ClipOutput> ClipService::tick(uint32_t now_ms, const dh_device_drops *drops) {
    std::vector<ClipOutput> outputs;
    dh_xfer_action actions[kActionCapacity];
    const std::string board = drops_line(drops);

    /* A question nobody answered. See kHoldTimeoutMs for why it cannot be left
       standing: the copy side re-offers every two seconds until it is
       requested, and the buffer the size cap sizes stays pinned. */
    if (have_held_offer_) {
        if (!held_timed_) {
            held_timed_ = true;
            held_since_ = now_ms;
        } else if (now_ms - held_since_ >= kHoldTimeoutMs) {
            const uint32_t id = held_offer_.id;
            append(outputs, decline_files(id));
            outputs.push_back(note("a file offer went unanswered for " +
                                   std::to_string(kHoldTimeoutMs / 1000u) +
                                   "s and was declined"));
        }
    } else {
        held_timed_ = false;
    }

    /* A size cap that changed while something was arriving (#56). Tried here
       rather than remembered for ever, because the transfer it waited on ends
       without telling anyone. */
    if (wanted_capacity_ != 0 && dh_xfer_can_set_rx_buffer(xfer_.get())) {
        std::vector<uint8_t> replacement(wanted_capacity_);
        if (dh_xfer_set_rx_buffer(xfer_.get(), replacement.data(), replacement.size())) {
            rx_buffer_ = std::move(replacement);
            outputs.push_back(note("the clipboard size cap that was waiting is now in force: " +
                                   std::to_string(wanted_capacity_ / (1024u * 1024u)) + " MB"));
            wanted_capacity_ = 0;
        }
    }

    if (!have_pending_ || seal_tx_.live) {
        seal_waiting_timed_ = false;
    } else if (!seal_waiting_timed_) {
        seal_waiting_timed_ = true;
        seal_waiting_since_ = now_ms;
        seal_retry_since_ = now_ms;
    } else if (now_ms - seal_waiting_since_ >= kStallTimeoutMs) {
        have_pending_ = false;
        pending_.clear();
        seal_waiting_timed_ = false;
        outputs.push_back(note("a copy waiting for a seal made no progress for " +
                               std::to_string(kStallTimeoutMs / 1000) +
                               "s and was abandoned (" + board + ")"));
    } else if (now_ms - seal_retry_since_ >= kSweepDelayMs) {
        seal_retry_since_ = now_ms;
        append(outputs, offer_seal());
    }

    /* Unsigned differences throughout, so a wrapping millisecond counter is
       arithmetic rather than a transfer abandoned once every 49 days. */
    if (!dh_xfer_is_sending(xfer_.get())) {
        offer_retry_timed_ = false;
    } else if (!offer_retry_timed_ || offer_retry_mark_ != tx_progress_) {
        offer_retry_timed_ = true;
        offer_retry_since_ = now_ms;
        offer_retry_mark_ = tx_progress_;
    } else if (dh_xfer_tx_awaiting_request(xfer_.get()) &&
               now_ms - offer_retry_since_ >= kSweepDelayMs) {
        offer_retry_since_ = now_ms;
        append(outputs, render(actions, dh_xfer_retry_offer(xfer_.get(), actions,
                                                            kActionCapacity)));
    }

    if (!dh_xfer_is_receiving(xfer_.get())) {
        receiving_timed_ = false;
    } else if (!receiving_timed_ || receiving_mark_ != dh_xfer_rx_received(xfer_.get())) {
        receiving_timed_ = true;
        receiving_since_ = now_ms;
        swept_since_ = now_ms;
        receiving_mark_ = dh_xfer_rx_received(xfer_.get());
    } else if (now_ms - receiving_since_ >= kStallTimeoutMs) {
        const std::string line = progress_line();
        receiving_timed_ = false;
        append(outputs, render(actions, dh_xfer_cancel_rx(xfer_.get(), actions, kActionCapacity)));
        outputs.push_back(note("an arriving transfer made no progress for " +
                               std::to_string(kStallTimeoutMs / 1000) + "s and was abandoned (" +
                               line + "; " + board + "); nothing partial is ever written"));
    } else if (now_ms - swept_since_ >= kSweepDelayMs) {
        swept_since_ = now_ms;
        append(outputs, sweep());
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
        {
        const bool awaiting = dh_xfer_tx_awaiting_request(xfer_.get());
        const uint32_t retries = dh_xfer_tx_offer_retries(xfer_.get());
        outputs = render(actions, dh_xfer_handle_request(xfer_.get(), id, actions,
                                                         kActionCapacity));
        append(outputs, pump());
        if (awaiting && !dh_xfer_tx_awaiting_request(xfer_.get()) && retries > 0)
            outputs.push_back(note("an offer succeeded after " + std::to_string(retries) +
                                   " retry action(s) were produced"));
        return outputs;
        }

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

/*
 * A fresh seal is the only word this end gets that the far helper's process
 * started over, because a helper offers one exactly when it holds no key to
 * send under. Its offer ids started over with it, so the offer-id frontier this
 * end measures them against belongs to a process that no longer exists (#151) —
 * kept, it would read the restarted helper's first offer as stale and leave the
 * clipboard dead in that direction until this end reset too.
 *
 * So the incoming direction is forgotten here. Anything half-arrived belonged
 * to the seal just replaced and can never be finished; it is abandoned rather
 * than delivered in part.
 *
 * A healthy receive cannot be thrown away this way. A duplicated frame never
 * reaches here — a counter seen once is refused for ever (dh_auth.h) — and a
 * genuinely fresh offer means the far end holds no key, which is exactly the
 * state in which it cannot be sending anything.
 */
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
        if (rc == DH_SEAL_OK) {
            std::vector<ClipOutput> outputs{send(DH_MSG_SEAL_ACCEPT, reply, written)};
            dh_xfer_action actions[kActionCapacity];
            const size_t n = dh_xfer_rx_seal_replaced(xfer_.get(), actions, kActionCapacity);
            append(outputs, render(actions, n));
            return outputs;
        }
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

    /*
     * A new offer starts a new deadline. Disarmed here rather than inferred in
     * the tick, because what the tick can see — the received count — is zero
     * for the transfer being replaced and zero for the one replacing it, and
     * the transfer id cannot separate them either: ids are per far-helper
     * *process* and start again at one when that process restarts. Without
     * this the second transfer inherits whatever is left of the first one's
     * thirty seconds and is abandoned seconds old (#145). No clock is read
     * here, which is what keeps one out of every path that produces an action.
     */
    dh_xfer_action actions[kActionCapacity];
    const bool had_offer = dh_xfer_rx_has_offer(xfer_.get());
    const uint32_t previous_id = dh_xfer_rx_offer_id(xfer_.get());
    if (offer.kind == static_cast<uint8_t>(ClipKind::Files))
        return on_file_offer(offer, had_offer, previous_id);
    const bool lazy = offer.kind == static_cast<uint8_t>(ClipKind::Png) &&
                      offer.total > kEagerImageThreshold;
    std::vector<ClipOutput> outputs = withdraw_held_offer(offer.id);
    append(outputs, render(
        actions, lazy ? dh_xfer_handle_offer_lazy(xfer_.get(), &offer, actions, kActionCapacity)
                      : dh_xfer_handle_offer(xfer_.get(), &offer, actions, kActionCapacity)));
    if (lazy) {
        const bool accepted = dh_xfer_rx_has_offer(xfer_.get()) &&
                              dh_xfer_rx_offer_id(xfer_.get()) == offer.id;
        bool protocol_error = false;
        for (const ClipOutput &output : outputs)
            if (output.kind == ClipOutput::Kind::ProtocolError) protocol_error = true;
        const bool duplicate = accepted && had_offer && previous_id == offer.id;
        const char *disposition = protocol_error ? "conflict"
                                  : !accepted      ? "rejected"
                                  : duplicate     ? "duplicate"
                                                  : "new";
        /* One duplicate establishes that retries reach this helper. The core's
           aggregate counter retains the total without flooding the log every
           two seconds (ADR-0009). */
        if (!duplicate || dh_xfer_rx_duplicate_offers(xfer_.get()) == 1)
            outputs.push_back(note("[clipboard-debug] lazy offer id=" +
                                   std::to_string(offer.id) + " bytes=" +
                                   std::to_string(offer.total) + " disposition=" + disposition +
                                   " previous_id=" +
                                   std::to_string(had_offer ? previous_id : 0)));
    }
    if (lazy && (!had_offer || previous_id != offer.id) && dh_xfer_rx_has_offer(xfer_.get()) &&
        dh_xfer_rx_offer_id(xfer_.get()) == offer.id) {
        ClipOutput out;
        out.kind = ClipOutput::Kind::LazyImage;
        out.transfer_id = offer.id;
        out.total = offer.total;
        lazy_image_id_ = offer.id;
        outputs.push_back(std::move(out));
    }
    if (!had_offer || dh_xfer_rx_offer_id(xfer_.get()) != previous_id)
        receiving_timed_ = false;
    return outputs;
}

/*
 * Files are offered from the other computer.
 *
 * Accepted into the transfer machine as **lazy** and then left there: the far
 * end learns its offer was heard and stops repeating it (#78), while not one
 * byte crosses the link until this computer's user says so. Small sets skip the
 * question — see kFilePromptThreshold for why a prompt for a quarter-second
 * transfer makes the prompt that matters worthless.
 *
 * Twin: ClipboardService.onFileOffer.
 */
std::vector<ClipOutput> ClipService::withdraw_held_offer(uint32_t superseded_by) {
    if (!have_held_offer_ || held_offer_.id == superseded_by) return {};
    ClipOutput withdrawn;
    withdrawn.kind = ClipOutput::Kind::FileOfferWithdrawn;
    withdrawn.transfer_id = held_offer_.id;
    have_held_offer_ = false;
    held_timed_ = false;
    have_incoming_files_ = false;
    incoming_files_.clear();
    std::vector<ClipOutput> outputs;
    outputs.push_back(std::move(withdrawn));
    return outputs;
}

std::vector<ClipOutput> ClipService::on_file_offer(const dh_clip_offer &offer, bool had_offer,
                                                   uint32_t previous_id) {
    std::vector<FileEntry> files;
    uint64_t listed = 0;
    if (!decode_file_list(offer.meta, offer.meta_len, files, listed)) {
        std::vector<ClipOutput> refused;
        refused.push_back(send(DH_MSG_CLIP_CANCEL, encode_id(offer.id)));
        refused.push_back(
            note("a file offer named files this helper will not write, so it was refused"));
        return refused;
    }
    /*
     * The list's own total, as the core summed it, against the total the same
     * offer promises. Both come from the same far helper, so a disagreement is
     * that helper being wrong or being tampered with — and either way this end
     * would otherwise write files by slicing a payload at offsets it has no
     * reason to trust.
     */
    if (listed != offer.total) {
        std::vector<ClipOutput> refused;
        refused.push_back(send(DH_MSG_CLIP_CANCEL, encode_id(offer.id)));
        refused.push_back(note("a file offer promised " + std::to_string(offer.total) +
                               " bytes and listed " + std::to_string(listed) +
                               ", so it was refused"));
        return refused;
    }

    /* An identical retry of the offer already being held is the far end
       repeating itself, not a second question to ask (#78, ADR-0009). */
    if (have_held_offer_ && held_offer_.id == offer.id && held_offer_.total == offer.total &&
        held_offer_.files == files)
        return {};

    std::vector<ClipOutput> outputs = withdraw_held_offer(offer.id);
    have_held_offer_ = false;
    held_timed_ = false;

    dh_xfer_action actions[kActionCapacity];
    append(outputs, render(actions, dh_xfer_handle_offer_lazy(xfer_.get(), &offer, actions,
                                                              kActionCapacity)));
    /*
     * Held, and held for *this* offer — the only state in which there is a
     * question to ask.
     *
     * Not `dh_xfer_rx_has_offer`, which was the bug: that reports `seen_offer`
     * and survives an offer the machine has already refused for being over the
     * size cap, so a 2.5 MB file against a 2 MB cap was put to the user and
     * accepting it did nothing. It also survives the answer, so every
     * two-second offer retry re-asked a question already answered — three
     * toasts for one file, observed on hardware.
     */
    if (!dh_xfer_rx_is_held(xfer_.get()) || dh_xfer_rx_offer_id(xfer_.get()) != offer.id)
        return outputs;

    if (!had_offer || previous_id != offer.id) {
        receiving_timed_ = false;
        swept_since_ = 0;
    }

    if (offer.total <= kFilePromptThreshold) {
        incoming_files_ = files;
        have_incoming_files_ = true;
        append(outputs, render(actions, dh_xfer_request_lazy(xfer_.get(), offer.id, actions,
                                                             kActionCapacity)));
        return outputs;
    }

    held_offer_ = deskhop::FileOffer{offer.id, offer.total, files};
    have_held_offer_ = true;
    /* Armed by the tick that follows, not here: no clock is read on a path
       that produces actions, which is the same rule the deadlines above
       follow. */
    held_timed_ = false;
    ClipOutput ask;
    ask.kind = ClipOutput::Kind::FileOffer;
    ask.transfer_id = offer.id;
    ask.total = offer.total;
    ask.files = files;
    outputs.push_back(std::move(ask));
    return outputs;
}

/*
 * Split a delivered file payload back into files.
 *
 * The list comes from the offer, checked against that offer's own total before
 * a single byte was asked for — so the sizes here are numbers already agreed,
 * not a second parse of anything the far end says now. The bounds check stays
 * all the same: a payload shorter than the list claims is a bug on this path,
 * and the answer to one is a refused delivery, never a short file presented as
 * whole.
 */
std::vector<ClipOutput> ClipService::deliver_files(const uint8_t *bytes, size_t len) {
    if (!have_incoming_files_)
        return {note("a file payload arrived with no list to split it by; nothing was written")};

    std::vector<FileEntry> files = std::move(incoming_files_);
    incoming_files_.clear();
    have_incoming_files_ = false;

    uint64_t listed = 0;
    for (const FileEntry &file : files) listed += file.size;
    if (listed != len)
        return {note("a file payload of " + std::to_string(len) + " bytes did not match the " +
                     std::to_string(listed) + " its list named; nothing was written")};

    ClipOutput out;
    out.kind = ClipOutput::Kind::DeliverFiles;
    out.payload_kind = static_cast<uint8_t>(ClipKind::Files);
    out.files = files;
    out.bytes.assign(bytes, bytes + len);
    std::vector<ClipOutput> outputs;
    outputs.push_back(std::move(out));
    outputs.push_back(note(std::to_string(files.size()) + " file(s), " + std::to_string(len) +
                           " bytes, arrived whole"));
    return outputs;
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

/// What a parked copy is, for the notes that report one waiting.
std::string ClipService::describe_pending() const {
    if (pending_is_files_) {
        return std::to_string(pending_files_.size()) + " file(s), " +
               std::to_string(pending_total_) + " bytes";
    }
    return std::to_string(pending_.size()) + " bytes";
}

std::vector<ClipOutput> ClipService::start_pending_if_sealed() {
    if (!have_pending_) return {};
    /*
     * No key yet, so the copy waits for one. Said out loud because a copy that
     * parks in silence reads at the desk as a copy that did nothing — and on a
     * link that is reconnecting, parking is the ordinary case, not the rare one.
     */
    if (!seal_tx_.live) {
        std::vector<ClipOutput> outputs = offer_seal();
        outputs.push_back(note(describe_pending() +
                               " copied here are waiting for a seal before anything is offered"));
        return outputs;
    }

    have_pending_ = false;
    seal_waiting_timed_ = false;
    dh_xfer_action actions[kActionCapacity];

    if (pending_is_files_) {
        /* Lazy: the list goes out, the bytes do not. `tx_meta_` outlives the
           offer because the core keeps a bare pointer into it. */
        outgoing_provider_ = std::move(pending_provider_);
        pending_provider_ = nullptr;
        tx_meta_ = std::move(pending_meta_);
        pending_meta_.clear();
        tx_payload_.clear();
        const size_t count = pending_files_.size();
        const uint64_t total = pending_total_;
        pending_files_.clear();
        std::vector<ClipOutput> outputs = render(
            actions, dh_xfer_offer(xfer_.get(), pending_kind_, tx_meta_.data(),
                                   static_cast<uint16_t>(tx_meta_.size()), nullptr, total,
                                   actions, kActionCapacity));
        outputs.push_back(note(std::to_string(count) + " file(s), " + std::to_string(total) +
                               " bytes, were offered without being read"));
        return outputs;
    }

    outgoing_provider_ = nullptr;
    tx_meta_.clear();
    tx_payload_ = std::move(pending_);
    pending_.clear();

    const size_t n = dh_xfer_offer(xfer_.get(), pending_kind_, nullptr, 0, tx_payload_.data(),
                                   tx_payload_.size(), actions, kActionCapacity);
    return render(actions, n);
}

std::vector<ClipOutput> ClipService::reoffer() {
    dh_clip_offer current{};
    if (!dh_xfer_offer_info(xfer_.get(), &current)) return {};

    dh_xfer_action actions[kActionCapacity];
    /* A lazy transfer whose payload was never asked for has no bytes to point
       at, and must not acquire any here: re-offering it eagerly would read the
       files at the moment a seal went stale rather than at the moment someone
       asked for them (#56). */
    if (outgoing_provider_ && tx_payload_.empty()) {
        return render(actions,
                      dh_xfer_offer(xfer_.get(), current.kind, tx_meta_.data(),
                                    static_cast<uint16_t>(tx_meta_.size()), nullptr,
                                    current.total, actions, kActionCapacity));
    }
    if (tx_payload_.empty()) return {};
    const size_t n = dh_xfer_offer(xfer_.get(), current.kind, nullptr, 0, tx_payload_.data(),
                                   tx_payload_.size(), actions, kActionCapacity);
    return render(actions, n);
}

std::vector<ClipOutput> ClipService::offer_seal() {
    if (aead_ == nullptr) {
        have_pending_ = false;
        pending_.clear();
        seal_waiting_timed_ = false;
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
    seal_waiting_timed_ = false;
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
        case DH_XFER_ACT_SEND_OFFER_RETRY:
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
            outputs.push_back(send(DH_MSG_CLIP_REQUEST, encode_id(action.id)));
            break;
        case DH_XFER_ACT_SEND_RETRANSMIT:
            outputs.push_back(
                send(DH_MSG_CLIP_RETRANSMIT, encode_retransmit(action.id, action.seq)));
            break;
        case DH_XFER_ACT_SEND_CREDIT:
            outputs.push_back(send(DH_MSG_CLIP_CREDIT, encode_credit(action.id, action.credits)));
            break;
        case DH_XFER_ACT_SEND_CANCEL:
            outputs.push_back(send(DH_MSG_CLIP_CANCEL, encode_id(action.id)));
            break;
        case DH_XFER_ACT_DELIVERED: {
            if (lazy_image_id_ == action.id) lazy_image_id_ = 0;
            const uint8_t kind = dh_xfer_delivered_kind(xfer_.get());
            const uint64_t length = dh_xfer_delivered_len(xfer_.get());
            const size_t bounded = static_cast<size_t>(
                length < rx_buffer_.size() ? length : rx_buffer_.size());
            if (kind == static_cast<uint8_t>(ClipKind::Files)) {
                append(outputs, deliver_files(rx_buffer_.data(), bounded));
                if (dh_xfer_rx_duplicate_offers(xfer_.get()) > 0)
                    outputs.push_back(
                        note("a transfer completed after " +
                             std::to_string(dh_xfer_rx_duplicate_offers(xfer_.get())) +
                             " duplicate offer(s) were observed"));
                break;
            }
            ClipOutput out;
            out.kind = ClipOutput::Kind::Deliver;
            out.payload_kind = kind;
            out.bytes.assign(rx_buffer_.begin(),
                             rx_buffer_.begin() + static_cast<ptrdiff_t>(bounded));
            outputs.push_back(std::move(out));
            if (dh_xfer_rx_duplicate_offers(xfer_.get()) > 0)
                outputs.push_back(note("a transfer completed after " +
                                       std::to_string(dh_xfer_rx_duplicate_offers(xfer_.get())) +
                                       " duplicate offer(s) were observed"));
            break;
        }
        case DH_XFER_ACT_FAILED:
            if (lazy_image_id_ == action.id) {
                lazy_image_id_ = 0;
                ClipOutput clear;
                clear.kind = ClipOutput::Kind::CancelLazyImage;
                clear.transfer_id = action.id;
                outputs.push_back(std::move(clear));
            }
            /*
             * Which direction failed is asked of the machine, not of the id:
             * ids are per direction and collide across the two (#136), so a
             * send that failed would otherwise throw away a healthy receive's
             * file list — and the receive would then arrive with nothing to
             * split it by.
             */
            if (!dh_xfer_rx_busy(xfer_.get())) {
                if (have_held_offer_) {
                    have_held_offer_ = false;
                    held_timed_ = false;
                    ClipOutput withdrawn;
                    withdrawn.kind = ClipOutput::Kind::FileOfferWithdrawn;
                    withdrawn.transfer_id = held_offer_.id;
                    outputs.push_back(std::move(withdrawn));
                }
                have_incoming_files_ = false;
                incoming_files_.clear();
            }
            if (!dh_xfer_is_sending(xfer_.get())) outgoing_provider_ = nullptr;
            outputs.push_back(note("transfer " + std::to_string(action.id) + " was abandoned: " +
                                   fail_reason(action.reason)));
            break;
        case DH_XFER_ACT_PROTOCOL_ERROR: {
            ClipOutput out;
            out.kind = ClipOutput::Kind::ProtocolError;
            out.note = "offer " + std::to_string(action.id) +
                       " reused immutable identity with different content";
            outputs.push_back(std::move(out));
            break;
        }
        case DH_XFER_ACT_NEED_DATA: {
            dh_xfer_action more[kActionCapacity];
            if (!outgoing_provider_) {
                outputs.push_back(note("a lazy payload was asked for and this end promised none"));
                append(outputs, render(more, dh_xfer_provide_fail(xfer_.get(), more,
                                                                  kActionCapacity)));
                break;
            }
            /*
             * Read here and nowhere earlier: this is the moment #56 exists
             * for, when someone on the other computer has said yes.
             *
             * It costs a pause and it costs memory, and both are bounded and
             * accepted rather than overlooked. The whole set is read into RAM
             * and then moved into the transfer's own storage, so a 64 MB paste
             * — the largest cap the board can state — peaks at roughly that.
             * The read happens on the window thread, which also carries the
             * heartbeat: at SSD speeds that is tens of milliseconds against a
             * three-second deadline, and input itself never touches this
             * helper, so what a long read could cost is one late cursor
             * placement. Streaming from disk instead would mean the transfer
             * core holding a callback rather than a pointer, which is a change
             * to the shared machine both helpers run.
             */
            std::vector<uint8_t> payload;
            const bool read = outgoing_provider_(payload);
            dh_clip_offer current{};
            const uint64_t promised =
                dh_xfer_offer_info(xfer_.get(), &current) ? current.total : 0;
            if (!read) {
                outgoing_provider_ = nullptr;
                outputs.push_back(note("the copied files could not be read, so the transfer was "
                                       "abandoned rather than sent short"));
                append(outputs, render(more, dh_xfer_provide_fail(xfer_.get(), more,
                                                                  kActionCapacity)));
                break;
            }
            /*
             * The offer promised a length and the core will read exactly that
             * many bytes from what it is given, so a short read here is an
             * overread there. It is also the ordinary case of a file edited
             * between the copy and the paste, which must fail the transfer
             * rather than truncate it.
             */
            if (payload.size() != promised) {
                outgoing_provider_ = nullptr;
                outputs.push_back(note("the copied files were " +
                                       std::to_string(payload.size()) + " bytes and " +
                                       std::to_string(promised) +
                                       " were offered, so the transfer was abandoned rather "
                                       "than sent short"));
                append(outputs, render(more, dh_xfer_provide_fail(xfer_.get(), more,
                                                                  kActionCapacity)));
                break;
            }
            tx_payload_ = std::move(payload);
            outputs.push_back(note("the copied files were read: " +
                                   std::to_string(tx_payload_.size()) + " bytes"));
            append(outputs, render(more, dh_xfer_provide(xfer_.get(), tx_payload_.data(), more,
                                                         kActionCapacity)));
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
