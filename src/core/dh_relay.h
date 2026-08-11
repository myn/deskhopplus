/*
 * deskhopplus shared core — the inter-board relay (#47).
 *
 * A frame handed to one board arrives intact at the other, having crossed the
 * inter-board serial link, without the firmware ever interpreting its
 * contents. The link carries 8 payload bytes per packet, so a frame is
 * fragmented: a start packet carrying the total length, then data packets
 * each carrying a full 8 bytes — no per-packet sequence number, which on an
 * 8-byte payload would cost 25% on top of the wire's own 33% framing
 * overhead (#32).
 *
 * The only thing read here is the frame header, and only for the two
 * decisions the firmware is allowed to make: which priority band the frame is
 * in, and whether it is well-formed enough to forward. Payloads are opaque —
 * that is what keeps a clipboard format change from ever requiring a device
 * to be reflashed.
 *
 * Pure C11, no I/O, no allocation: the caller owns every buffer.
 */

#ifndef DH_RELAY_H_
#define DH_RELAY_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dh_frame.h"

/* One inter-board packet carries this many payload bytes (PACKET_DATA_LENGTH). */
#define DH_RELAY_PAYLOAD 8u

/*
 * Consecutive bulk packets emitted before the pump yields. The inter-board
 * queue is shared with keyboard and mouse traffic, so a 4 KiB frame's 512
 * packets must not be enqueued in one go — bulk never starves input.
 */
#define DH_RELAY_BURST_MAX 8u

/* Session and placement frames are small; this bounds the priority slot. */
#define DH_RELAY_PRIORITY_MAX 128u

typedef enum {
    DH_RELAY_PKT_START = 0, /* data[0..1] = total frame length, u16 LE */
    DH_RELAY_PKT_DATA = 1,  /* data[0..7] = the next bytes of the frame */
} dh_relay_pkt_kind;

typedef struct {
    uint8_t kind; /* dh_relay_pkt_kind */
    uint8_t data[DH_RELAY_PAYLOAD];
    uint8_t len; /* meaningful bytes in data; always DH_RELAY_PAYLOAD on the
                    wire, since a short packet would cost a length field */
} dh_relay_packet;

typedef enum {
    DH_RELAY_OK = 0,
    DH_RELAY_AGAIN = 1,        /* reassembly incomplete */
    DH_RELAY_ERR_BUSY = -1,    /* that band's slot still holds a frame */
    DH_RELAY_ERR_OVERSIZE = -2,/* longer than the slot or the frame maximum */
    DH_RELAY_ERR_FRAME = -3,   /* not a well-formed frame header */
    DH_RELAY_ERR_ORPHAN = -4,  /* data with no start: discarded */
    DH_RELAY_ERR_TRUNCATED = -5, /* a start arrived mid-frame: the previous
                                    frame lost packets and is abandoned */
} dh_relay_result;

/* One band's frame in flight. Storage is the caller's. */
typedef struct {
    uint8_t *buf;
    uint16_t cap;
    uint16_t len;  /* 0 when idle */
    uint16_t sent; /* bytes already emitted in data packets */
    bool started;  /* the start packet has been emitted */
} dh_relay_slot;

/*
 * Two slots, because priority must be able to overtake bulk that is merely
 * queued. It does not overtake bulk that is in flight: one reassembly context
 * per direction means interleaved fragments would be spliced into each
 * other's frames. A bulk frame is a chunk, so the wait is bounded by the
 * chunk size rather than by the whole transfer.
 */
typedef struct {
    dh_relay_slot priority;
    dh_relay_slot bulk;
    uint16_t burst;         /* consecutive bulk packets since the last yield */
    uint32_t refused;       /* frames refused because a slot was busy */
} dh_relay_tx;

void dh_relay_tx_init(dh_relay_tx *t, uint8_t *priority_buf, uint16_t priority_cap,
                      uint8_t *bulk_buf, uint16_t bulk_cap);

/*
 * Take a complete frame for relaying. The band comes from one comparison on
 * the type byte. Refuses rather than truncates: a frame too long for the slot,
 * a malformed header, or a band whose slot is still draining. A refusal is
 * the backpressure — the caller must not drop it silently (#43).
 */
dh_relay_result dh_relay_tx_offer(dh_relay_tx *t, const uint8_t *frame, size_t len);

/*
 * The next packet owed to the wire, without consuming it. Returns false when
 * nothing is owed or the burst cap has been reached for this pump.
 *
 * Peek/commit rather than pop: an enqueue that fails leaves the packet owed
 * rather than lost, so a full transmit queue costs a retry instead of a hole
 * in a frame that would corrupt everything after it.
 */
bool dh_relay_tx_peek(dh_relay_tx *t, dh_relay_packet *out);

/* The peeked packet reached the queue. */
void dh_relay_tx_commit(dh_relay_tx *t);

/* Start of a new pump: clears the burst counter. */
void dh_relay_tx_yield(dh_relay_tx *t);

/* True while any frame is in flight — the caller's "not now" for new offers. */
bool dh_relay_tx_busy(const dh_relay_tx *t);

/* Reassembly, one context per direction. Storage is the caller's. */
typedef struct {
    uint8_t *buf;
    uint16_t cap;
    uint16_t expected; /* 0 when no frame is in progress */
    uint16_t have;
    uint32_t orphans;   /* data packets discarded for want of a start */
    uint32_t truncated; /* frames abandoned because packets went missing */
} dh_relay_rx;

void dh_relay_rx_init(dh_relay_rx *r, uint8_t *buf, uint16_t cap);

/*
 * Feed one inter-board packet. On DH_RELAY_OK a complete frame is in *out,
 * viewing the reassembly buffer until the next push. The error results are
 * counted internally as well as returned, so a caller that ignores them still
 * leaves the loss visible.
 */
dh_relay_result dh_relay_rx_push(dh_relay_rx *r, const dh_relay_packet *packet,
                                 dh_frame_view *out);

#endif /* DH_RELAY_H_ */
