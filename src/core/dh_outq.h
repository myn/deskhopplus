/*
 * deskhopplus shared core — the device's outbound frame queue (#69).
 *
 * Both of the device's outbound seams hand whole frames to a transport that
 * drains them in small fixed units: this board's helper takes 64 bytes per
 * 1 ms HID report, the peer board 8 bytes per inter-board packet. Each seam
 * used to hold exactly one frame and refuse the next outright — and with no
 * retransmit beneath either of them, a refusal was silent truncation. A 4 KiB
 * frame owns a seam for ~64 ms of USB drain, and the 3.6 Mbaud inter-board
 * link completes frames far faster than that, so the collision was the
 * expected case under load rather than an edge. This is the queue both grew.
 *
 * Two bands: a session reply must not wait out a clipboard frame. Behind the
 * bulk band, a bounded FIFO — deep enough to absorb the burst one slow drain
 * creates, and no deeper, because a *sustained* overrun is the credit window's
 * problem and the credit window is end-to-end between helpers, not something
 * the firmware may read (ADR-0003, ADR-0005).
 *
 * The invariant worth stating plainly: at most one frame is ever mid-drain,
 * and it finishes before any other frame's first byte. Both seams feed a
 * reader that recovers frame boundaries from the byte stream alone — the
 * helper's dh_frame_reader, the peer's single dh_relay_rx context — so
 * interleaving two frames desynchronises it. Priority therefore overtakes bulk
 * that is merely queued, never bulk that is in flight.
 *
 * Header bytes only, never payload: the band comes from one comparison on the
 * type byte, which is the whole of what the firmware is allowed to know about
 * a relayed frame.
 *
 * Pure C11, no I/O, no allocation, no locking — a caller reached from two
 * cores locks around it, as channel.c does.
 */

#ifndef DH_OUTQ_H_
#define DH_OUTQ_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dh_frame.h"

/* C++ links these symbols too — the Windows helper is C++ (#49). */
#ifdef __cplusplus
extern "C" {
#endif

/*
 * The priority band holds one frame. Session, placement and pairing frames are
 * small — the largest the board emits is a 76-byte pairing grant
 * (DH_SESSION_REPLY_MAX) — and rare enough that two in flight at once has
 * never been reachable.
 */
#define DH_OUTQ_PRIORITY_MAX 128u

/*
 * A frame waiting behind the one in flight. Sized for the largest bulk frame a
 * transfer can actually complete with — not for the 4 KiB frame maximum, which
 * would cost four times the RAM to hold traffic that never arrives in bursts
 * (ADR-0005):
 *
 *   CLIP_CHUNK  4 + 24 + 40 (DH_SEAL_CHUNK_OVERHEAD) + 1024 (DH_XFER_CHUNK_SIZE) = 1092
 *   CLIP_OFFER  4 + 24 + 43 (DH_SEAL_OFFER_OVERHEAD) + 1024 (DH_XFER_META_MAX)   = 1095
 *
 * The 24 is v2's authentication prefix (#111): it sits inside the frame's
 * length, so every frame this band holds on the helper-facing side grew by it.
 * Missing that would not have failed loudly — an over-long bulk frame is
 * refused as retryable rather than rejected — it would simply have stopped
 * anything queueing *behind* the frame in flight, which is the whole reason
 * this band exists. Frames crossing the inter-board link carry no prefix, so
 * they are 24 bytes smaller and comfortably inside the same bound.
 *
 * The seal (#113) is inside these figures for the same reason the prefix is:
 * what crosses this seam is the sealed body, not the clear one. This bound was
 * stated for the clear sizes while the transfer core had yet to seal anything,
 * and #52 made that untrue without moving it — so from then until #135 a
 * full-size chunk was 24 bytes over the bound and could never queue behind
 * anything at all. Small copies fitted in one short chunk and worked; a
 * clipboard payload past DH_XFER_CHUNK_SIZE lost most of itself, silently,
 * counted only in a field that cannot be read (#133).
 *
 * The offer matters as much as the chunk and is the one that sets this bound.
 * A chunk lost here is re-requested end to end, but there is no retransmit for
 * an offer — its loss costs the whole transfer, out to the helper's timeout —
 * so an offer that cannot queue is the expensive kind of refusal. An offer
 * carrying more metadata than DH_XFER_META_MAX is cancelled by the receiver on
 * arrival, so beyond this bound there is no frame worth reserving space for.
 *
 * outq_test.c pins both figures against the transfer machine's and the seal's
 * own constants, so the firmware need not include either to state its own
 * buffer size — and so that neither can move again without this failing.
 */
#define DH_OUTQ_STAGE_MAX 1095u

/* Frames queued behind the one in flight. */
#define DH_OUTQ_DEPTH 2u

typedef enum {
    DH_OUTQ_OK = 0,
    DH_OUTQ_ERR_BUSY = -1,     /* no room now; retryable */
    DH_OUTQ_ERR_OVERSIZE = -2, /* longer than its band can ever hold */
    DH_OUTQ_ERR_FRAME = -3,    /* not a well-formed frame header */
} dh_outq_result;

/* What the transport owes next. Valid until the next call that mutates. */
typedef struct {
    const uint8_t *at;   /* the next unsent byte */
    uint16_t remaining;  /* unsent bytes of this frame */
    uint16_t total;      /* the whole frame's length */
    bool bulk;           /* which band owes it — the relay's burst cap needs this */
    /*
     * True until dh_outq_note_preamble is called for this frame. The seam
     * between "a queue of frames" and a transport that prefixes each frame
     * with something of its own: the relay owes a start packet carrying
     * `total` before any data packet, and only the queue knows where one frame
     * ends and the next begins. A byte-stream transport ignores this.
     */
    bool preamble_owed;
} dh_outq_view;

/* One band's progress through the frame it holds. */
typedef struct {
    uint16_t len;        /* 0 when the band holds nothing */
    uint16_t sent;
    bool preamble_done;
} dh_outq_band;

typedef struct {
    dh_outq_band priority;
    uint8_t priority_buf[DH_OUTQ_PRIORITY_MAX];

    /* The bulk frame in flight, then the ring of frames waiting to become it. */
    dh_outq_band bulk;
    uint8_t bulk_buf[DH_FRAME_MAX_SIZE];
    uint8_t stage[DH_OUTQ_DEPTH][DH_OUTQ_STAGE_MAX];
    uint16_t stage_len[DH_OUTQ_DEPTH];
    uint8_t stage_first; /* the oldest queued frame */
    uint8_t stage_used;

    uint32_t refused; /* frames the queue could not take; saturates */
} dh_outq;

void dh_outq_init(dh_outq *q);

/*
 * Take a complete frame. Refuses rather than truncates, and counts every
 * refusal, so a frame the queue cannot carry is never silently lost (#43).
 * DH_OUTQ_ERR_BUSY is retryable — including for a bulk frame too long for a
 * queued slot, which fits again once the in-flight buffer is free.
 */
dh_outq_result dh_outq_offer(dh_outq *q, const uint8_t *frame, size_t len);

/* What the transport owes next, without consuming it. False when nothing is
   owed. Peek/advance rather than pop: a transport whose own queue refuses the
   bytes leaves them owed rather than lost. */
bool dh_outq_peek(const dh_outq *q, dh_outq_view *out);

/*
 * n bytes of the peeked frame reached the transport. Clamped to what that
 * frame still owes; completing a frame promotes the next one behind it.
 *
 * The view is passed back rather than re-derived, because a caller may release
 * its lock around the transport's own write — channel.c does, rather than call
 * into TinyUSB inside a critical section — and a frame offered from the other
 * core in that gap can change which band is owed *next*. Crediting the bytes
 * to the band they actually came from is what makes that gap safe.
 */
void dh_outq_advance(dh_outq *q, const dh_outq_view *view, uint16_t n);

/* The peeked frame's preamble reached the transport. */
void dh_outq_note_preamble(dh_outq *q, const dh_outq_view *view);

/* True while any frame is still owed, in either band. */
bool dh_outq_busy(const dh_outq *q);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DH_OUTQ_H_ */
