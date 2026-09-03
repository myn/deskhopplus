/*
 * deskhopplus shared core — the chunked transfer state machine (#48).
 *
 * Ported from mkroamer's clip-transfer core and extended with the reliability
 * model (#32): CRC32 per chunk, selective retransmission, a credit window,
 * and abandon-on-drop. Semantics: docs/protocol.md, "Transfer semantics".
 *
 * Pure logic: messages in, actions out, no I/O, no allocation. The caller
 * owns every buffer; decoded views must stay valid only for the call. Runs
 * end-to-end between the helpers — the firmware never instantiates it.
 */

#ifndef DH_XFER_H_
#define DH_XFER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dh_clip.h"

/* C++ links these symbols too — the Windows helper is C++ (#49). */
#ifdef __cplusplus
extern "C" {
#endif

/* Working values; the chunk size is a build constant until #39 measures,
   then the hello negotiates the effective value. */
#define DH_XFER_CHUNK_SIZE 1024u

/*
 * How many chunks the receiver lets the sender have outstanding.
 *
 * **It is sized against the board's outbound queue, not against the link.**
 * The board may not read a payload (ADR-0003), so it cannot enforce a credit
 * of its own; dh_outq.h says outright that a *sustained* overrun "is the
 * credit window's problem". This is that window, and it is the only
 * back-pressure the path has.
 *
 * It was 16 while the queue holds one frame in flight and DH_OUTQ_DEPTH behind
 * it. The receiver granted all 16 up front, the sender emitted all 16 in one
 * pump, and everything past the third was refused. A refused chunk is
 * re-requested; a refused CLIP_OFFER or CLIP_DONE has no retransmit and costs
 * the whole transfer out to a thirty-second timeout (#78) — which at the desk
 * is a copy that does not arrive until you make it again (#137). Measured on
 * hardware 2026-08-25: 16 refusals on the receiving board across a handful of
 * copies, once #133 made that counter readable.
 *
 * The queue holds the window **plus one**, not the window: dh_xfer_pump emits
 * the CLIP_DONE in the same batch as the last chunks and does not credit-gate
 * it, so a queue sized to the window alone refuses exactly the frame nothing
 * retransmits. DH_OUTQ_DEPTH is 3 for that reason (#141), and closing the gap
 * from this side instead was tried three ways — a window of 2, a ceiling on
 * accumulated credit, a pump batch capped at 2 — each of which stalls
 * double-loss recovery, because the covering grants that pay for
 * retransmissions are what a cap discards.
 *
 * Three is not a throughput figure either. The drain is the bottleneck at
 * ~64 KB/s, and three chunks outstanding covers about 48 ms of it — more than
 * a credit's round trip, so the sender still never waits. What it stops is the
 * sender running four times ahead of a seam that then throws the difference
 * away. outq_test pins it against DH_OUTQ_DEPTH so the two cannot drift apart
 * again; raising it means deepening that queue first, which costs board RAM
 * and a flash.
 */
#define DH_XFER_CREDIT_WINDOW 3u

#define DH_XFER_BATCH_MAX 16u   /* max chunk actions emitted per pump */
#define DH_XFER_RETX_MAX 64u    /* pending retransmit requests held by the sender */
#define DH_XFER_MAX_CHUNKS 65536u /* received-set bound: 64 MiB at 1 KiB chunks */
#define DH_XFER_META_MAX 1024u  /* copied offer metadata the receiver retains */
/* Immutable identity also covers refused metadata across the codec's complete
   uint16_t namespace, so an identical retry can be distinguished from conflict. */
#define DH_XFER_IDENTITY_META_MAX DH_CLIP_OFFER_META_WIRE_MAX

typedef enum {
    DH_XFER_ACT_SEND_OFFER,      /* sender: send the offer (dh_xfer_offer_at) */
    DH_XFER_ACT_NEED_DATA,       /* sender: lazy payload requested — call provide */
    DH_XFER_ACT_SEND_CHUNK,      /* sender: send chunk .seq (dh_xfer_chunk_at) */
    DH_XFER_ACT_SEND_DONE,       /* sender: send CLIP_DONE for .id */
    DH_XFER_ACT_SEND_REQUEST,    /* receiver: request offered .id */
    DH_XFER_ACT_SEND_RETRANSMIT, /* receiver: re-request chunk .seq of .id */
    DH_XFER_ACT_SEND_CREDIT,     /* receiver: grant .credits */
    DH_XFER_ACT_SEND_CANCEL,     /* either side: cancel .id */
    DH_XFER_ACT_DELIVERED,       /* receiver: payload complete in rx_buf */
    DH_XFER_ACT_FAILED,          /* transfer abandoned; .reason says why */
    DH_XFER_ACT_SEND_OFFER_RETRY, /* sender: repeat immutable current offer */
    DH_XFER_ACT_PROTOCOL_ERROR,  /* authenticated offer identity conflict */
} dh_xfer_action_type;

typedef enum {
    DH_XFER_FAIL_CANCELLED = 0,
    DH_XFER_FAIL_LINK_DROP = 1,
    DH_XFER_FAIL_NO_DATA = 2, /* lazy provider could not produce the payload */
    DH_XFER_FAIL_SEAL_REPLACED = 3, /* the copy side's helper started over */
} dh_xfer_fail_reason;

typedef struct {
    uint8_t type;   /* dh_xfer_action_type */
    uint8_t reason; /* dh_xfer_fail_reason, for FAILED */
    uint16_t credits;
    uint32_t id;
    uint32_t seq;
} dh_xfer_action;

typedef struct {
    /* outgoing transfer */
    struct {
        bool active;
        bool streaming;    /* a request started chunk emission */
        bool lazy_pending; /* waiting on provide() */
        bool need_done;    /* all chunks emitted; CLIP_DONE owed */
        uint32_t id;
        uint8_t kind;
        const uint8_t *meta;
        uint16_t meta_len;
        const uint8_t *data;
        uint64_t total;
        uint32_t nchunks;
        uint32_t next_seq;
        uint32_t credits;
        uint32_t retx[DH_XFER_RETX_MAX]; /* ring of re-requested seqs */
        uint32_t retx_head, retx_count;
        bool opening_credit_accepted; /* this transfer spent its one opening grant */
        bool recovery_credit_due; /* a retransmit request's covering grant is expected */
        /* What this end was asked to send again, and what it sent again. Read
           together they answer the question a stall could not answer before
           (#145): the receiver asked, and this end did or did not act on it. */
        uint32_t retx_asked;
        uint32_t retx_sent;
        uint32_t offer_retries;
    } tx;
    /* incoming transfer */
    struct {
        bool active;
        bool lazy; /* accepted offer, waiting for a paste-side request */
        bool seen_offer; /* identity remains after completion/refusal */
        uint32_t id;
        uint8_t kind;
        uint64_t total;
        uint32_t nchunks;
        uint32_t nreceived;
        uint32_t max_seq_seen;
        bool any_seen;
        uint32_t ungranted; /* valid chunks since the last credit grant */
        /* The same pair from the receiving end: what this end asked for again,
           and how much of it came back. A stall with `asked` at zero is a
           receiver that never noticed; one with `answered` at zero is a far end
           that is not acting on what it was asked (#145). */
        uint32_t retx_asked;
        uint32_t retx_answered;
        uint32_t duplicate_offers;
        uint16_t meta_len;
        uint8_t meta[DH_XFER_IDENTITY_META_MAX]; /* copied: offer view is transient */
        uint8_t received[DH_XFER_MAX_CHUNKS / 8];
        /* seqs re-requested and still outstanding, with a one-round age bit:
           a DONE sweep skips-but-ages a fresh request (its retransmission is
           still in flight behind that DONE), and re-requests one that has
           survived a whole round — so a lost retransmission converges while
           each loss is reported once per round */
        uint8_t requested[DH_XFER_MAX_CHUNKS / 8];
        uint8_t requested_stale[DH_XFER_MAX_CHUNKS / 8];
    } rx;
    uint8_t *rx_buf;
    size_t rx_cap;
    uint32_t next_id;
} dh_xfer;

/* rx_buf/rx_cap: where incoming payloads assemble; an offer larger than
   rx_cap is refused with a cancel. */
void dh_xfer_init(dh_xfer *x, uint8_t *rx_buf, size_t rx_cap);

/*
 * Point the receive buffer somewhere else, without disturbing anything else
 * the machine holds.
 *
 * The buffer is sized against the clipboard size cap, and the board is the
 * single source of truth for that cap (#42, #56) — so it is stated per
 * session and can change while a helper is running. Allocating the 64 MB
 * maximum against a 10 MB default would hold six times the memory the helper
 * can ever use, which on the managed laptop this runs on is not free.
 *
 * False while anything is arriving, which is the whole of the rule. A payload
 * mid-assembly would be truncated and delivered as complete; a *lazy* offer
 * that is merely being held is refused for a sharper reason still, as its
 * total was measured against the old capacity and nothing measures it again
 * when the request goes out. The caller re-offers the change once the
 * transfer ends.
 *
 * The sending direction is untouched on purpose. Offer ids are ordered in this
 * helper's own namespace, and restarting them mid-session is precisely what
 * the far end reads as this process having restarted (#151) — so this is not
 * dh_xfer_init with a different buffer.
 */
bool dh_xfer_set_rx_buffer(dh_xfer *x, uint8_t *rx_buf, size_t rx_cap);

/*
 * Whether anything at all is incoming — a payload assembling, or a lazy offer
 * being held for a decision. Wider than dh_xfer_is_receiving, which excludes
 * the held case.
 *
 * It is what a caller needs to answer "is my incoming transfer over?" after a
 * FAILED action, because a transfer id cannot answer it: ids are per direction
 * and collide across the two (#136), so a failure of the outgoing transfer
 * would otherwise be read as the incoming one's and throw its state away.
 */
static inline bool dh_xfer_rx_busy(const dh_xfer *x) { return x->rx.active; }

/*
 * Whether an offer is being *held* for a decision: accepted into the machine,
 * nothing requested, not one byte moving.
 *
 * This is the question a caller must ask before putting an offer to its user,
 * and `dh_xfer_rx_has_offer` is not it. That one reports `seen_offer`, which is
 * set even for an offer this machine has just **refused** — one past the size
 * cap sets the id, emits a cancel, and leaves `active` false. A caller that
 * asked the wrong one showed the user a question about a transfer that had
 * already been declined, and accepting it did nothing at all (#56).
 *
 * It is also what makes a duplicate offer silent after the answer: once the
 * request has gone out the receive is no longer lazy, so the copy side's next
 * retry cannot re-ask a question the user has already answered.
 */
static inline bool dh_xfer_rx_is_held(const dh_xfer *x) { return x->rx.active && x->rx.lazy; }

/* Whether that swap would be accepted right now. Asked *before* a caller
   allocates the replacement: a helper that retries a refused swap on every
   tick would otherwise allocate and free up to 64 MB several times a second
   for the whole of the transfer it is waiting on. */
static inline bool dh_xfer_can_set_rx_buffer(const dh_xfer *x) { return !dh_xfer_rx_busy(x); }

/*
 * Every call below returns the number of actions written to acts (at most
 * acts_cap). DH_XFER_BATCH_MAX + 2 suffices for every call except
 * dh_xfer_handle_done, whose sweep wants one action per missing chunk plus
 * a grant: it truncates safely at acts_cap, and truncated requests repeat
 * at the next DONE round. Frame payloads for the SEND_* actions are built
 * with dh_clip.h plus the accessors below.
 */

/* Offer a payload. data == NULL offers lazily: the payload is fetched via
   NEED_DATA → dh_xfer_provide only if the peer requests it. A new offer
   supersedes any outgoing transfer in flight. */
size_t dh_xfer_offer(dh_xfer *x, uint8_t kind, const uint8_t *meta, uint16_t meta_len,
                     const uint8_t *data, uint64_t total, dh_xfer_action *acts,
                     size_t acts_cap);
/* Answer NEED_DATA. data must hold the total promised in the offer and stay
   valid until the transfer ends. */
size_t dh_xfer_provide(dh_xfer *x, const uint8_t *data, dh_xfer_action *acts, size_t acts_cap);
/* Answer NEED_DATA when the payload cannot be produced. */
size_t dh_xfer_provide_fail(dh_xfer *x, dh_xfer_action *acts, size_t acts_cap);
/* Emit the next credit-gated batch of chunk actions (retransmits first).
   Call whenever the bulk band has room; empty when nothing is owed. */
size_t dh_xfer_pump(dh_xfer *x, dh_xfer_action *acts, size_t acts_cap);
/* Caller-timed recovery for an offer still awaiting its first request. */
size_t dh_xfer_retry_offer(dh_xfer *x, dh_xfer_action *acts, size_t acts_cap);
/* Cancel the outgoing transfer locally (user abort on the copy side). */
size_t dh_xfer_cancel_tx(dh_xfer *x, dh_xfer_action *acts, size_t acts_cap);
/* Cancel the incoming transfer locally (user abort on the paste side). */
size_t dh_xfer_cancel_rx(dh_xfer *x, dh_xfer_action *acts, size_t acts_cap);
/* The link dropped: abandon both directions. Partial data is never kept. */
size_t dh_xfer_link_down(dh_xfer *x, dh_xfer_action *acts, size_t acts_cap);

/*
 * The copy side offered a fresh seal: forget the incoming direction, ordering
 * included.
 *
 * Offer ids are ordered inside the *copy side helper's* namespace and start
 * again at one when that process does, so this end's own session is not the
 * only boundary of that namespace (#151). The far helper can restart while
 * this side stays healthy, and its first offer would then be measured against
 * the dead process's offer-id frontier and dropped as stale — a clipboard that stays
 * unavailable in that direction until this end happens to reset too.
 *
 * A fresh incoming seal is that boundary, and it is the only evidence this end
 * gets: a helper offers a seal only when it holds no key to send under. Any
 * incomplete receive belongs to the seal it arrived under and can never be
 * finished by a helper that has forgotten it, so it is abandoned whole and no
 * partial payload is delivered.
 *
 * The outgoing direction is deliberately untouched. What this end is sending
 * recovers by the ordinary SEAL_STALE exchange, which re-offers it under a key
 * the far end can open.
 */
size_t dh_xfer_rx_seal_replaced(dh_xfer *x, dh_xfer_action *acts, size_t acts_cap);

/* Peer messages in. */
size_t dh_xfer_handle_offer(dh_xfer *x, const dh_clip_offer *offer, dh_xfer_action *acts,
                            size_t acts_cap);
size_t dh_xfer_handle_offer_lazy(dh_xfer *x, const dh_clip_offer *offer,
                                 dh_xfer_action *acts, size_t acts_cap);
size_t dh_xfer_request_lazy(dh_xfer *x, uint32_t id, dh_xfer_action *acts,
                            size_t acts_cap);
size_t dh_xfer_handle_request(dh_xfer *x, uint32_t id, dh_xfer_action *acts, size_t acts_cap);
/* A receive completes here, on the chunk that fills the received-set, with
   DH_XFER_ACT_DELIVERED — not at CLIP_DONE (#132). Every chunk is verified on
   arrival, so the receiver can see for itself that it is finished, and a DONE
   refused by the device's bounded outbound queue then costs nothing. A
   zero-length offer is the exception: it has no chunks, so nothing reaches
   here and only CLIP_DONE can complete it. */
size_t dh_xfer_handle_chunk(dh_xfer *x, const dh_clip_chunk *chunk, dh_xfer_action *acts,
                            size_t acts_cap);
/* Drives the retransmit sweep for an incomplete receive. It still completes a
   transfer that already has every chunk, which since #132 means a zero-length
   one — nothing else reaches here with the set full — plus the unreached case
   of a DELIVERED the chunk handler had no room to emit. */
size_t dh_xfer_handle_done(dh_xfer *x, uint32_t id, dh_xfer_action *acts, size_t acts_cap);

size_t dh_xfer_handle_cancel(dh_xfer *x, uint32_t id, dh_xfer_action *acts, size_t acts_cap);
size_t dh_xfer_handle_retransmit(dh_xfer *x, uint32_t id, uint32_t seq, dh_xfer_action *acts,
                                 size_t acts_cap);
size_t dh_xfer_handle_credit(dh_xfer *x, uint32_t id, uint16_t credits, dh_xfer_action *acts,
                             size_t acts_cap);

/*
 * The receiver's own prompt: ask again for what an arriving transfer is
 * waiting on. Empty when nothing is arriving.
 *
 * Every message that would otherwise restart a stalled receive — a credit
 * grant, a retransmit request, the CLIP_DONE that drives the sweep above —
 * crosses the same seams the payload does, and a seam that refuses one has no
 * retransmit beneath it (ADR-0005). Losing any of them used to cost the whole
 * transfer out to the helper's thirty-second timeout, at no consistent size
 * and no consistent fraction (#145). So the receiver stops depending on being
 * told and works it out for itself, which is the same move #132 made when a
 * receive stopped waiting for CLIP_DONE to say it was finished.
 *
 * It has no clock and must not gain one. **The caller's tick decides when**:
 * call this on a receive that has made no progress for a short interval — well
 * over a round trip, well under the stall timeout. Each helper's clipboard
 * service already measures exactly that.
 *
 * What it asks for, in one message batch:
 *   - CLIP_REQUEST again, when nothing has arrived at all. The request may have
 *     been lost, or the grant covering it, or the opening burst of chunks
 *     itself; this end cannot tell which, and a sender already streaming
 *     ignores the repeat.
 *   - every hole below the highest seq seen: those chunks were sent and lost.
 *   - up to a window's worth at and above that seq — or from seq 0 when nothing
 *     has arrived. Past there the sender may simply not have got that far, so
 *     asking for the whole tail would be one request per remaining chunk. A
 *     window is what it could have had in flight.
 * Every request carries its covering credit, as at any other seam, so a sender
 * stopped by a lost grant is paid to start again.
 */
size_t dh_xfer_sweep_rx(dh_xfer *x, dh_xfer_action *acts, size_t acts_cap);

/*
 * Whether a transfer is live in each direction.
 *
 * There is no clock in here and there must not be one — this is pure logic,
 * messages in and actions out. A stall is therefore the *caller's* to notice:
 * a helper whose peer stopped answering (its far helper crashed, say) sees no
 * message at all, so nothing here can fire. These two are what a caller needs
 * to know there is something to give up on. See the transfer timeout in each
 * helper's clipboard service.
 */
static inline bool dh_xfer_is_sending(const dh_xfer *x) { return x->tx.active; }
static inline bool dh_xfer_is_receiving(const dh_xfer *x) {
    return x->rx.active && !x->rx.lazy;
}

/*
 * How far each direction has got. A stall says nothing useful without these:
 * "no progress" covers a transfer whose chunks never arrived and one whose
 * chunks all arrived and were refused, and those have nothing in common.
 *
 * `rx_received` against `rx_chunks` is the one that separates them — a chunk
 * this machine refuses (wrong transfer, sequence out of range, a CRC32 that
 * does not match) is dropped with no action, so a caller comparing this before
 * and after is the only way to see it happen at all.
 */
static inline uint32_t dh_xfer_rx_chunks(const dh_xfer *x) { return x->rx.nchunks; }
static inline uint32_t dh_xfer_rx_received(const dh_xfer *x) { return x->rx.nreceived; }
static inline uint32_t dh_xfer_tx_chunks(const dh_xfer *x) { return x->tx.nchunks; }
static inline uint32_t dh_xfer_rx_retx_asked(const dh_xfer *x) { return x->rx.retx_asked; }
static inline uint32_t dh_xfer_rx_retx_answered(const dh_xfer *x) { return x->rx.retx_answered; }
static inline uint32_t dh_xfer_tx_retx_asked(const dh_xfer *x) { return x->tx.retx_asked; }
static inline uint32_t dh_xfer_tx_retx_sent(const dh_xfer *x) { return x->tx.retx_sent; }
static inline uint32_t dh_xfer_tx_offer_retries(const dh_xfer *x) { return x->tx.offer_retries; }
static inline uint32_t dh_xfer_rx_duplicate_offers(const dh_xfer *x) {
    return x->rx.duplicate_offers;
}
static inline uint32_t dh_xfer_tx_next_seq(const dh_xfer *x) { return x->tx.next_seq; }
static inline bool dh_xfer_tx_streaming(const dh_xfer *x) { return x->tx.streaming; }
static inline bool dh_xfer_tx_awaiting_request(const dh_xfer *x) {
    return x->tx.active && !x->tx.streaming && !x->tx.lazy_pending;
}
static inline bool dh_xfer_rx_has_offer(const dh_xfer *x) { return x->rx.seen_offer; }
static inline uint32_t dh_xfer_rx_offer_id(const dh_xfer *x) { return x->rx.id; }

/* Fill in the wire form of the current outgoing offer / one of its chunks
   (computing the chunk's CRC32). False when there is no such transfer. */
bool dh_xfer_offer_info(const dh_xfer *x, dh_clip_offer *out);
bool dh_xfer_chunk_at(const dh_xfer *x, uint32_t seq, dh_clip_chunk *out);

/* The delivered payload after DELIVERED: what is in rx_buf, plus the copied
   offer metadata. Valid until the next incoming offer. */
static inline uint64_t dh_xfer_delivered_len(const dh_xfer *x) { return x->rx.total; }
static inline uint8_t dh_xfer_delivered_kind(const dh_xfer *x) { return x->rx.kind; }
static inline const uint8_t *dh_xfer_delivered_meta(const dh_xfer *x, uint16_t *len) {
    *len = x->rx.meta_len;
    return x->rx.meta;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DH_XFER_H_ */
