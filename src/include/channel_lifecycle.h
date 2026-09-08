#ifndef CHANNEL_LIFECYCLE_H_
#define CHANNEL_LIFECYCLE_H_

#include "dh_inq.h"
#include "dh_relay.h"
#include "usb_descriptors.h"
#include "dh_outq.h"
#include "dh_session.h"
#include "dh_txq.h"

/* Firmware lifecycle ownership, using the existing protocol and queue storage.
   Core 0 owns the session and pairing; the adapter serializes outbound access.
   Entropy, persistence and USB callbacks remain in channel.c. */
/*
 * Reports waiting for channel_task, because none of the work below may run
 * inside the USB callback any more.
 *
 * This was four, on the argument that "USB full speed delivers at most one
 * 64-byte OUT report per millisecond and channel_task drains the whole ring at
 * 1000 Hz, so the steady state never exceeds one". That is the same premise
 * #139 measured false one queue over (dh_inq.h): channel_task is an entry in a
 * cooperative loop, not a 1000 Hz timer, and TinyUSB hands over whatever it
 * has buffered when the device task runs — so reports arrive in bursts inside
 * one pass, not one per millisecond.
 *
 * Thirty-two covers a loop pass of about 32 ms. It is not a measurement,
 * because nobody has measured the worst pass; it is generous depth behind a
 * drop that is no longer silent, which is the change that matters. See
 * `stream_broken` — a dropped report now ends the session, so if this
 * number is still too small it says so instead of costing three seconds of
 * confusion.
 *
 * The old comment also said a lost report "loses a frame ... the helper
 * retries". It does not. The reader is a byte stream, and a gap in the middle
 * of a frame is filled from the frames after it; `frame_test.c`'s
 * `test_a_gap_mid_frame_is_not_recoverable` is what that costs.
 */
#define CHANNEL_REPORT_BACKLOG 32u

typedef enum { CURSOR_QUERY_NONE, CURSOR_QUERY_LOCAL, CURSOR_QUERY_PEER } channel_query_origin;

typedef struct {
    dh_session session;
    dh_pair pair;
    dh_outq out;
    dh_txq_stats tx;
    dh_frame_reader reader;

    /*
     * Reports from tud_hid_set_report_cb, drained by channel_task.
     *
     * No lock, and that is a property of the scheduler rather than an
     * omission: tud_hid_set_report_cb is reached from usb_device_task and
     * channel_task is another entry in the *same* cooperative loop on core 0
     * (src/main.c), so the two can never interleave. Core 1 does not touch it.
     */
    uint8_t reports[CHANNEL_REPORT_BACKLOG][CHANNEL_REPORT_SIZE];
    uint16_t report_len[CHANNEL_REPORT_BACKLOG];
    uint8_t report_head; /* next to drain */
    uint8_t report_used;
    uint32_t reports_dropped;
    /*
     * A report was dropped, so the byte stream has a hole in it (#161).
     *
     * Raised in the USB callback and acted on in channel_task, the same shape
     * as `config_wiped` and for the same reason: ending a session sends a
     * frame, and no decision may run at the bottom of a TinyUSB callback.
     */
    bool stream_broken;
    /* Every report the USB callback delivered, dropped ones included. The head
       of the inbound chain, so a helper writing frames the board never accepts
       can be told apart from one whose frames never arrived (#107). */
    uint32_t reports_in;

    /* The relay to and from the peer board; the transmitter carries its own
       storage, the reassembler takes ours. */
    dh_relay_tx relay_tx;
    dh_relay_rx relay_rx;
    uint8_t relay_rx_buf[DH_FRAME_MAX_SIZE];

    /*
     * Frames reassembled from the peer board, waiting for core 0 to write this
     * board's tag over them.
     *
     * The tag is per hop, so a frame arriving from the peer board has to be
     * authenticated under *this* board's k_b2h with *this* board's counter —
     * and both belong to the session, which is core 0's. Core 1 could not do
     * it without sharing the counter across cores, and a counter allocated on
     * one core and used on the other can emit out of order, which the far end
     * refuses as a replay. So the frame is handed over and core 0 tags it.
     *
     * Same shape, and the same reason, as the deferred configuration wipe. This was one
     * slot on the argument that "a frame takes about 4 ms to arrive over a
     * 3.6 Mbaud link and core 0 drains at 1000 Hz", which is true of a
     * full-size chunk and of nothing else — see dh_inq.h, and #139 for what it
     * measured on both boards.
     */
    dh_inq inbound;

    /*
     * The same frame with this board's prefix written in front of it, on its
     * way to the outbound queue.
     *
     * Static rather than a local, and that is not a style choice: core 0's
     * stack is 2 KB (PICO_STACK_SIZE) inside a 4 KB SCRATCH_Y whose neighbour
     * is core 1's, and a relayed frame can be 4100 bytes. A buffer this size
     * on that stack overruns both — which is the same reasoning that keeps the
     * reply buffer report-sized rather than frame-sized, applied to the one
     * place where a whole frame genuinely has to be assembled.
     */
    uint8_t tagged[DH_FRAME_MAX_SIZE];

    /* A registration that channel_task still owes the configuration. */
    bool registration_unsaved;

    /* A source-position query handed from core 1 to core 0. Protected by the
       outbound lock so a peer request cannot overwrite a local one. */
    channel_query_origin cursor_query_origin;
    uint8_t cursor_query_id;

} channel_lifecycle;

/* Hardware adapter: same outbound critical section as transport peek/advance. */
void channel_lifecycle_lock(void);
void channel_lifecycle_unlock(void);

bool channel_lifecycle_queue(channel_lifecycle *c, const uint8_t *frame, size_t len,
                             uint32_t now);
/* Session-band frame only. Owns successful hello -> fresh stream -> reply.
   Registration changes remain visible to the persistence adapter via pair. */
void channel_lifecycle_on_frame(channel_lifecycle *c, const dh_frame_view *frame,
                                uint32_t now);

void channel_lifecycle_link_lost(channel_lifecycle *c);

void channel_lifecycle_receive_report(channel_lifecycle *c, const uint8_t *buffer, uint16_t len);
void channel_lifecycle_config_wiped(channel_lifecycle *c, uint32_t now);
/* One clock for decoding and liveness, with hardware work between them. */
void channel_lifecycle_step(channel_lifecycle *c, uint32_t now, void *context);

/* Platform effects: pointer/config state, cross-core inbound handoff and UART. */
void channel_lifecycle_position(void *context, const uint8_t *body, size_t len);
void channel_lifecycle_update_config(void *context, uint32_t now);
bool channel_lifecycle_send_relay(const dh_relay_packet *packet);

bool channel_lifecycle_emit_placement(channel_lifecycle *c, uint8_t type,
                                      const uint8_t *body, size_t len, uint32_t now);
void channel_lifecycle_barrier(void);
void channel_lifecycle_save_registration(void *context);
bool channel_lifecycle_query_unavailable(void *context, uint8_t query_id);

#endif
