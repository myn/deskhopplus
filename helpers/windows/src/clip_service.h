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

#include "dh_seal.h"
#include "dh_session.h"
#include "dh_xfer.h"

namespace deskhop {

/* The payload kinds on the wire (docs/protocol.md, CLIP_OFFER). Only text
   travels in this slice; images are #55 and files are #56. */
enum class ClipKind : uint8_t { Text = 0, Png = 1, Files = 2 };

struct ClipOutput {
    enum class Kind {
        Send,    /* a frame body for the session to authenticate and send */
        Deliver, /* a complete payload, for this computer's clipboard */
        Note,    /* diagnostics, never shown to the user */
    };

    Kind kind{Kind::Note};
    uint8_t type{0};         /* Send: the message type */
    uint8_t payload_kind{0}; /* Deliver: ClipKind */
    std::vector<uint8_t> bytes;
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

    /*
     * How long a transfer may make no progress before it is given up on.
     *
     * Enormously more than the link needs: a full credit window is 16 KB, which
     * at this transport's ~64 KB/s per direction is a quarter of a second. The
     * margin is deliberate — the cost of waiting too long is a stalled transfer
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
    std::vector<ClipOutput> on_chunk(const uint8_t *body, size_t len);

    std::vector<ClipOutput> start_pending_if_sealed();
    std::vector<ClipOutput> offer_seal();
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

    /*
     * The stall timeout's bookkeeping. The counters say what each direction has
     * actually done; the stamps say when that count last moved. Counting rather
     * than time-stamping inside `render` is what keeps a clock out of every
     * code path that produces an action.
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
    bool sending_timed_{false};
    uint32_t sending_since_{0};
    uint32_t sending_mark_{0};
    bool receiving_timed_{false};
    uint32_t receiving_since_{0};
    uint32_t receiving_mark_{0};
    uint32_t swept_since_{0};
};

} // namespace deskhop
