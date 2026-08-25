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
} dh_xfer_action_type;

typedef enum {
    DH_XFER_FAIL_CANCELLED = 0,
    DH_XFER_FAIL_LINK_DROP = 1,
    DH_XFER_FAIL_NO_DATA = 2, /* lazy provider could not produce the payload */
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
    } tx;
    /* incoming transfer */
    struct {
        bool active;
        uint32_t id;
        uint8_t kind;
        uint64_t total;
        uint32_t nchunks;
        uint32_t nreceived;
        uint32_t max_seq_seen;
        bool any_seen;
        uint32_t ungranted; /* valid chunks since the last credit grant */
        uint16_t meta_len;
        uint8_t meta[DH_XFER_META_MAX]; /* copied: the offer's view is transient */
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
/* Cancel the outgoing transfer locally (user abort on the copy side). */
size_t dh_xfer_cancel_tx(dh_xfer *x, dh_xfer_action *acts, size_t acts_cap);
/* Cancel the incoming transfer locally (user abort on the paste side). */
size_t dh_xfer_cancel_rx(dh_xfer *x, dh_xfer_action *acts, size_t acts_cap);
/* The link dropped: abandon both directions. Partial data is never kept. */
size_t dh_xfer_link_down(dh_xfer *x, dh_xfer_action *acts, size_t acts_cap);

/* Peer messages in. */
size_t dh_xfer_handle_offer(dh_xfer *x, const dh_clip_offer *offer, dh_xfer_action *acts,
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
static inline bool dh_xfer_is_receiving(const dh_xfer *x) { return x->rx.active; }

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
static inline uint32_t dh_xfer_tx_next_seq(const dh_xfer *x) { return x->tx.next_seq; }
static inline bool dh_xfer_tx_streaming(const dh_xfer *x) { return x->tx.streaming; }

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
