#pragma once
/*
 * The clipboard path, from a local copy to a frame body and back (#52).
 *
 * Everything it decides is already decided elsewhere: what a transfer does is
 * `dh_xfer`, what a seal does is `dh_seal`, and whether a direction is allowed
 * is the board's. What this file owns is the *joining* of those three — which
 * seal a payload waits on, which message carries which action, and what happens
 * to a transfer when a toggle changes underneath it.
 *
 * No Win32 here at all, which is the point: the clipboard itself
 * (clipboard.cpp) is the only file that touches the API, and everything that
 * can be got wrong about ordering, credit and seals is reachable by a test.
 *
 * The macOS twin is ClipboardService.swift. Two of these existing is
 * deliberate, and it is the same trade HelperSession already makes: the rules
 * both ends must agree on have one implementation in the shared core, and only
 * the joining is written twice. Two *transfer machines* would have given the
 * credit window and the received-set two chances to be got right.
 *
 * Single-threaded by construction, like the rest of the helper.
 */

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "dh_file_list.h"
#include "dh_seal.h"
#include "dh_session.h"
#include "dh_xfer.h"

namespace deskhop {

/* The payload kinds on the wire (docs/protocol.md, CLIP_OFFER). */
enum class ClipKind : uint8_t { Text = 0, Png = 1, Files = 2 };

/* One file in a transfer: what it is called, and how many bytes of the payload
   belong to it. Twin: FileListEntry.swift. */
struct FileEntry {
    std::string name;
    uint64_t size{0};

    bool operator==(const FileEntry &other) const {
        return name == other.name && size == other.size;
    }
};

/* Files the other computer has offered, waiting for this computer's user to say
   yes (#56). Nothing crosses the link until they do. */
struct FileOffer {
    uint32_t id{0};
    uint64_t total{0};
    std::vector<FileEntry> files;

    /* How long the transfer will take at the route's measured rate, in
       seconds — stated because a cap the user can raise to something taking a
       quarter of an hour deserves a duration in the dialog, not just a size. */
    uint32_t estimated_seconds() const;
};

/* Files that arrived whole. The bytes are every file's contents run together in
   `files` order; the transfer carries no boundaries of its own, which is what
   the list is for. */
struct FileDelivery {
    std::vector<FileEntry> files;
    std::vector<uint8_t> bytes;
};

struct ClipOutput {
    enum class Kind {
        Send,    /* a frame body for the session to authenticate and send */
        Deliver, /* a complete payload, for this computer's clipboard */
        LazyImage, /* install a paste-triggered image placeholder */
        CancelLazyImage,
        FileOffer,          /* ask the user: nothing has crossed the link yet */
        FileOfferWithdrawn, /* that question no longer stands */
        DeliverFiles,       /* a complete set, to be written and referenced */
        Note,    /* diagnostics, never shown to the user */
        TellUser, /* a message the user needs to see: something they did
                     produced nothing, and only they can act on why */
        ProtocolError, /* authenticated identity conflict: drop connection */
    };

    Kind kind{Kind::Note};
    uint8_t type{0};         /* Send: the message type */
    uint8_t payload_kind{0}; /* Deliver: ClipKind */
    uint32_t transfer_id{0}; /* LazyImage */
    uint64_t total{0};       /* LazyImage, FileOffer */
    std::vector<uint8_t> bytes;
    /* FileOffer and DeliverFiles: what the transfer names. */
    std::vector<FileEntry> files;
    std::string note;
};

class ClipService {
  public:
    /*
     * The largest payload this helper will assemble. The spec's default cap is
     * 10 MB; an offer above it is refused by the transfer core with a cancel
     * rather than truncated, so the far end learns why.
     */
    static constexpr size_t kDefaultCapacity = 10u * 1024u * 1024u;
    static constexpr size_t kEagerImageThreshold = 256u * 1024u;

    /*
     * Files at or below this size are accepted without asking. At the route's
     * measured rate they are a fraction of a second, and a dialog for a
     * quarter-second transfer is a dialog the user learns to dismiss without
     * reading — which is how the one that matters gets dismissed too.
     */
    static constexpr uint64_t kFilePromptThreshold = 1024u * 1024u;

    /*
     * The end-to-end rate #39 measured, in bytes per second, which is what
     * turns a size into a duration in the prompt. Deliberately the measured
     * figure rather than the arithmetic ~64 KB/s the transport was specified
     * at: the prompt exists so the user can decide whether to wait, and an
     * estimate a quarter short of the truth is worse than none.
     */
    static constexpr uint64_t kMeasuredBytesPerSecond = 33u * 1024u;

    /*
     * How long a file offer waits for an answer before it is declined for the
     * user.
     *
     * It has to expire, and not because the question is urgent. An offer
     * accepted-as-lazy and never requested leaves the *copy* side awaiting a
     * request, which it asks for again every two seconds for as long as the
     * session lasts (#78) — so an ignored prompt is a frame every two seconds
     * for ever. It also pins the receive buffer, which is what the size cap
     * sizes, so the cap cannot change while the question stands.
     *
     * Two minutes is human-scaled: long enough to finish a sentence and look
     * up, short enough that walking away from the desk costs a re-copy rather
     * than a link that never goes quiet.
     */
    static constexpr uint32_t kHoldTimeoutMs = 120000;

    /*
     * How long an arriving transfer may make no progress before it is given up on.
     *
     * Enormously more than the link needs: a full credit window is 16 KB, which
     * at this transport's ~64 KB/s per direction is a quarter of a second. The
     * margin is deliberate — the cost of waiting too long is a stalled receive
     * reported late, and the cost of firing too early is abandoning a healthy
     * one, which is the worse of the two.
     */
    static constexpr uint32_t kStallTimeoutMs = 30000;

    /*
     * How long an arriving transfer may make no progress before this end asks
     * again for what it is waiting on.
     *
     * Well over a round trip on this link — a credit grant and the chunk it
     * pays for cross in tens of milliseconds — and well under kStallTimeoutMs,
     * so a receive that can be recovered is recovered long before the deadline
     * that reports it lost.
     *
     * A receiver has to be able to prompt itself. Every message that would
     * otherwise restart it — a credit grant, a retransmit request, the
     * CLIP_DONE that drives a sweep — crosses the same seams the payload does,
     * and a seam that refuses one has no retransmit beneath it (ADR-0005).
     * Before #145 losing any of them cost the whole transfer, at no consistent
     * size and no consistent fraction.
     */
    static constexpr uint32_t kSweepDelayMs = 2000;

    /*
     * `aead` is the platform's cipher — seal_aead() over CNG — and must outlive
     * this object. Null is a machine that cannot run the seal at all, and this
     * refuses every copy rather than falling back to sending in clear.
     *
     * `entropy` must fill exactly the length it is asked for: it feeds an
     * ephemeral key, a nonce and a seal id, so a short draw would key a seal on
     * bytes nobody chose.
     */
    ClipService(const dh_seal_aead *aead, std::function<void(uint8_t *, size_t)> entropy,
                size_t capacity = kDefaultCapacity);

    /* Something was copied here. Eager: the bytes go now, so that pasting on
       the other computer never waits for a round trip. */
    std::vector<ClipOutput> local_copy(ClipKind kind, const std::vector<uint8_t> &bytes);

    /*
     * Files were copied here (#56). Lazy: what goes out now is the list and
     * nothing else, and `provider` is called only if the other computer's user
     * accepts the transfer — so copying a folder you never paste costs one
     * small frame and never opens a file.
     *
     * `provider` fills its argument with every file's contents run together in
     * `files` order and returns true, or returns false when they can no longer
     * be read. The length it produces must equal what was offered; a file
     * edited between the copy and the paste is a failed transfer rather than a
     * truncated one.
     */
    std::vector<ClipOutput> local_copy_files(
        const std::vector<FileEntry> &files,
        std::function<bool(std::vector<uint8_t> &)> provider);

    /* The user accepted the files the other computer offered. This is where a
       file transfer actually begins — on a decision made here, never on the
       copy made over there (ADR-0011). */
    std::vector<ClipOutput> accept_files(uint32_t id);
    /* The user declined. The far end is told, so its offer stops repeating. */
    std::vector<ClipOutput> decline_files(uint32_t id);
    /* The user gave up on a transfer already running. Nothing partial is ever
       delivered, so this loses the whole of it. */
    std::vector<ClipOutput> abort_receive();
    std::vector<ClipOutput> abort_send();

    /*
     * The board stated the clipboard size cap (#56). The receive buffer is
     * sized against it and cannot move while anything is arriving, so a change
     * landing mid-transfer is remembered and applied on the tick after that
     * transfer ends.
     */
    std::vector<ClipOutput> capacity_changed(uint8_t megabytes);

    /* What is arriving, for a progress display. False when nothing is. */
    bool arriving(uint8_t *kind, uint64_t *received, uint64_t *total) const;
    /* The file offer waiting on this computer's user, or null. */
    const deskhop::FileOffer *awaiting_decision() const;
    /* Whether anything is still on its way out of this computer. */
    bool awaiting_send() const { return dh_xfer_is_sending(xfer_.get()); }

    /* The board stated its clipboard policy (DH_CLIP_MAY_*). A direction turned
       off takes any transfer already crossing it with it. */
    std::vector<ClipOutput> policy_changed(uint8_t flags);

    /* The session went away. Both halves of the seal go with it — #107 measured
       586 teardowns in sixteen hours, and re-offering is cheap where a key whose
       peer may no longer exist is not. */
    std::vector<ClipOutput> session_ended();

    /* A chance to push more chunks — after every arriving frame, and on the
       tick. Empty when nothing is owed, which is the ordinary answer. */
    std::vector<ClipOutput> pump();
    std::vector<ClipOutput> request_lazy_image(uint32_t id);
    std::vector<ClipOutput> lazy_image_was_replaced(uint32_t id);

    /*
     * Give up on a transfer that has stopped moving.
     *
     * The other two interruptions #52 names — an unplug and a config-mode
     * reboot — both end this helper's own session, and `session_ended` already
     * abandons everything. The third does not: when the helper on the *other*
     * computer crashes, this one's session is perfectly healthy and simply
     * stops being answered. No message arrives, so nothing message-driven can
     * fire, and without this the transfer would sit holding its payload until
     * the next copy happened to supersede it.
     *
     * Measured against *progress*, not against the transfer's total duration: a
     * large payload legitimately takes minutes on this link, and a deadline on
     * the whole transfer would abandon healthy ones. `now_ms` is the same
     * monotonic clock the session runs on, compared as an unsigned difference.
     */
    /*
     * `drops` is what the board has said it dropped (#133), quoted into an
     * abandonment note; null means it has stated nothing this session, which
     * is a different answer from all-zero. A stall on a board that has been
     * losing frames is a different fault from a stall with clean seams, and
     * the numbers are only worth reading at the moment one happens — which is
     * here. Passed on every tick rather than held, because the board restates
     * them whenever they move and nothing tells this service when that was.
     */
    std::vector<ClipOutput> tick(uint32_t now_ms, const dh_device_drops *drops = nullptr);

    /* One verified bulk frame from the far helper. */
    std::vector<ClipOutput> received(uint8_t type, const uint8_t *body, size_t len);

    bool may_send() const { return may_send_; }
    bool may_receive() const { return may_receive_; }

    /* Every seam the board says has lost something, named, plus the inbound
       chain that is not a loss at all. Seams that have lost nothing are left
       out, so the ordinary line says so in three words and a line with
       anything in it is all signal.

       Public, and static, because the eviction path in main.cpp quotes it at
       the instant a session ends — the counterpart of macOS's BoardDrops.line,
       which is public for the same reason (#107). */
    static std::string drops_line(const dh_device_drops *drops);

  private:
    std::vector<ClipOutput> on_seal_offered(const uint8_t *body, size_t len);
    std::vector<ClipOutput> on_seal_accepted(const uint8_t *body, size_t len);
    std::vector<ClipOutput> on_seal_stale(const uint8_t *body, size_t len);
    std::vector<ClipOutput> on_offer(const uint8_t *body, size_t len);
    std::vector<ClipOutput> on_file_offer(const dh_clip_offer &offer, bool had_offer,
                                          uint32_t previous_id);
    /* A newer offer replaces whatever was arriving, so a question being held
       about the old one no longer stands. Needed on the *non-file* path too,
       which is what makes it worth having once: without it the tray goes on
       offering Accept for a transfer the far end has already moved past. */
    std::vector<ClipOutput> withdraw_held_offer(uint32_t superseded_by);
    std::vector<ClipOutput> deliver_files(const uint8_t *bytes, size_t len);
    std::vector<ClipOutput> on_chunk(const uint8_t *body, size_t len);

    std::vector<ClipOutput> start_pending_if_sealed();
    std::string describe_pending() const;
    std::vector<ClipOutput> offer_seal();
    std::vector<ClipOutput> resend_seal_offer();
    std::vector<ClipOutput> stale_reply(uint8_t type, const uint8_t *body, size_t len);
    std::vector<ClipOutput> render(const dh_xfer_action *actions, size_t count);
    std::vector<ClipOutput> sealed_offer();
    std::vector<ClipOutput> sealed_chunk(uint32_t seq);

    /* Ask again for what a stopped receive is waiting on, and say so. Said out
       loud on every round rather than counted quietly, because a stall that
       recovers is otherwise invisible: the transfer completes and nothing in
       the log says the link lost anything. */
    std::vector<ClipOutput> sweep();

    /* Offer the payload already in flight again, as a fresh transfer — what a
       SEAL_STALE costs. The bytes do not move; only the transfer around them
       starts over. */
    std::vector<ClipOutput> reoffer();

    void draw(uint8_t *out, size_t len);

    /* How far each direction has got. A stall that says only "no progress"
       cannot tell a transfer whose chunks never arrived from one whose chunks
       all arrived and were refused, and those have nothing in common. */
    std::string progress_line() const;

    const dh_seal_aead *aead_;
    std::function<void(uint8_t *, size_t)> entropy_;

    dh_seal_tx seal_tx_{};
    dh_seal_rx seal_rx_{};
    std::unique_ptr<dh_xfer> xfer_;
    std::vector<uint8_t> rx_buffer_;
    /* The payload on its way out. Held here rather than in the core because the
       core keeps a bare pointer into it for the transfer's whole life. */
    std::vector<uint8_t> tx_payload_;

    /*
     * The board's answer, and the default until it has given one. Both allowed,
     * matching what the toggles default to — a helper that refused until told
     * would refuse every copy made in the second before the policy frame
     * arrives, which reads at the desk as the clipboard not working.
     */
    bool may_send_{true};
    bool may_receive_{true};

    /* A copy waiting for a seal. Exactly one: a second copy while the first is
       still waiting supersedes it, because what the user last copied is what
       they mean to paste. */
    bool have_pending_{false};
    uint8_t pending_kind_{0};
    std::vector<uint8_t> pending_;

    /* A transfer that was already on its way out when the far helper said it
       holds no key for the seal. Once a fresh seal is accepted it has to start
       again, rather than carry on into a far end that never saw its offer. */
    bool reoffer_when_sealed_{false};
    /* The exchange, made idempotent under retransmission (#161): the offer this
       end is still waiting on, and the answer it already gave to an offer, both
       repeated verbatim rather than derived again. */
    /* The board's clipboard size cap, as the copy side needs it. Zero until the
       board has said, and nothing is refused on a cap nobody stated. */
    size_t cap_bytes_{0};
    uint8_t cap_megabytes_{0};
    std::vector<uint8_t> outstanding_offer_;
    std::vector<uint8_t> answered_offer_;
    std::vector<uint8_t> last_accept_;
    uint32_t lazy_image_id_{0};

    /*
     * The outgoing file transfer (#56).
     *
     * `tx_meta_` outlives the offer because the core keeps a bare pointer into
     * it, exactly as `tx_payload_` does. `outgoing_provider_` has a different
     * lifetime from `pending_provider_`: a pending copy ends when the offer
     * goes out, and the provider ends when the transfer does — NEED_DATA can
     * arrive minutes later, once someone on the other computer has said yes.
     */
    bool pending_is_files_{false};
    std::vector<FileEntry> pending_files_;
    std::vector<uint8_t> pending_meta_;
    uint64_t pending_total_{0};
    std::function<bool(std::vector<uint8_t> &)> pending_provider_;
    std::function<bool(std::vector<uint8_t> &)> outgoing_provider_;
    std::vector<uint8_t> tx_meta_;

    /*
     * The incoming half: an offer being *held* for an answer, and the list of
     * the transfer now arriving so that what is delivered can be split back
     * into files without parsing the metadata twice.
     */
    bool have_held_offer_{false};
    deskhop::FileOffer held_offer_;
    /* When the offer now being held was first put to the user. */
    bool held_timed_{false};
    uint32_t held_since_{0};
    bool have_incoming_files_{false};
    std::vector<FileEntry> incoming_files_;

    /* A size cap change waiting for the link to go quiet; zero means none. */
    size_t wanted_capacity_{0};

    /*
     * Seal-wait, receive-timeout and offer-retry bookkeeping. A copy waiting
     * on a seal has no transfer yet, so it owns a separate terminal stamp and
     * two-second retry stamp. A retry deliberately moves only the latter.
     *
     * The counters below say what each transfer direction has actually done;
     * the stamps say when that count last moved.
     * Counting rather than time-stamping inside `render` is what keeps a clock
     * out of every code path that produces an action.
     *
     * The receiving side counts *arrivals* — chunks assembled — and not the
     * messages this end emits. A sweep emits messages, so counting those would
     * let a receive whose far end is gone reset its own deadline for ever
     * (#145). The count alone cannot see a *supersede*, though — a newer offer
     * replaces an incomplete transfer and resets the count to zero, leaving the
     * mark unchanged — so `on_offer` disarms the deadline outright and the next
     * tick arms a fresh one.
     */
    uint32_t tx_progress_{0};
    bool offer_retry_timed_{false};
    uint32_t offer_retry_mark_{0};
    uint32_t offer_retry_since_{0};
    bool seal_waiting_timed_{false};
    uint32_t seal_waiting_since_{0};
    uint32_t seal_retry_since_{0};
    bool receiving_timed_{false};
    uint32_t receiving_since_{0};
    uint32_t receiving_mark_{0};
    uint32_t swept_since_{0};
};

} // namespace deskhop
