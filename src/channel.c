/*
 * The board's end of the helper channel (#45), its per-frame authentication
 * (#111) and its relay across the inter-board link (#47). See include/channel.h.
 */

#include "main.h"

#include <pico/critical_section.h>
#include <pico/rand.h>

#include "dh_inq.h"
#include "dh_outq.h"
#include "dh_pair.h"
#include "dh_place.h"
#include "dh_relay.h"
#include "dh_session.h"
#include "dh_txq.h"

#ifdef DH_DEV_NO_AUTH
#define CHANNEL_BUILD_TYPE DH_BUILD_DEVELOPMENT
#else
#define CHANNEL_BUILD_TYPE DH_BUILD_RELEASE
#endif

/*
 * Reports waiting for channel_task, because none of the work below may run
 * inside the USB callback any more.
 *
 * Four is not a throughput figure. USB full speed delivers at most one 64-byte
 * OUT report per millisecond and channel_task drains the whole ring at 1000 Hz,
 * so the steady state never exceeds one. It is depth for the one place where
 * this loop stalls on purpose: the 133 ms ECDH at pairing (#110), during which
 * nothing here runs at all. A report that does not fit is counted, and its
 * frame is lost the same way any refused frame is — the helper retries.
 */
#define CHANNEL_REPORT_BACKLOG 4u

static struct {
    dh_session session;
    dh_pair pair;
    dh_frame_reader reader;
    dh_txq_stats tx; /* replies lost to a busy endpoint, never silently */

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
    /* Every report the USB callback delivered, dropped ones included. The head
       of the inbound chain, so a helper writing frames the board never accepts
       can be told apart from one whose frames never arrived (#107). */
    uint32_t reports_in;

    /*
     * Outbound to this board's helper, emitted a report per tick. Session
     * replies and frames relayed from the peer share the one byte stream, so
     * they share the queue's band discipline: a reply overtakes bulk that is
     * merely queued, never bulk already on the stream (#69).
     */
    dh_outq out;

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
     * Same shape, and the same reason, as config_wiped below. This was one
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

    /*
     * The outbound queue is written from both cores: core 0 in the pump, core
     * 1 in the inter-board packet handler. An interleaving there splices two
     * frames into one byte stream, which the helper reads as a framing error
     * and drops the session over.
     */
    critical_section_t out_lock;
    bool locked;

    /*
     * A configuration wipe waiting to be applied. Both wipe paths run on
     * core 1 — the chord arrives through the USB *host* stack
     * (tuh_hid_report_received_cb → process_keyboard_report), the peer
     * board's WIPE_CONFIG_MSG through the packet receiver — while the session
     * lives on core 0. So the wipe is recorded here and applied by
     * channel_task, keeping the session and the registration rewritten only on
     * core 0. Doing it inline would race a hello mid-answer and could leave the
     * session re-established against a registration the wipe had just erased,
     * which is the very defect this exists to close (#75).
     */
    volatile bool config_wiped;

    /* A registration that channel_task still owes the configuration. */
    bool registration_unsaved;

    /* A source-position query handed from core 1 to core 0. Protected by the
       outbound lock so a peer request cannot overwrite a local one. */
    enum { CURSOR_QUERY_NONE, CURSOR_QUERY_LOCAL, CURSOR_QUERY_PEER } cursor_query_origin;
    uint8_t cursor_query_id;
} channel;

static bool channel_queue_frame(const uint8_t *frame, size_t len);
static bool channel_emit_placement_body(uint8_t type, const uint8_t *body, size_t body_len);
static bool channel_emit_placement_query(const uint8_t *place_body, uint8_t query_id);

static uint32_t channel_now_ms(void) {
    /*
     * Milliseconds off the 64-bit timer, so this counter uses the full uint32
     * range and wraps every ~49 days — which is what the session's wrap-safe
     * comparisons expect. time_us_32() / 1000 would not do: it spans only
     * 0..4,294,967 and jumps back to zero every ~72 minutes, and that
     * discontinuity reads as a difference of ~4.29e9 ms, marking a healthy
     * helper absent with no way back.
     */
    return to_ms_since_boot(get_absolute_time());
}

/*
 * Entropy for the two things this board must draw for itself: its identity
 * private key, once for the life of the board, and the session nonce in every
 * hello ack. pico_rand seeds from ring-oscillator jitter, the bus performance
 * counters and the board id.
 *
 * A guessable nonce would let a listener derive the session keys it is
 * otherwise locked out of, and a guessable private key would hand it the
 * board's identity outright — so this is the one place the whole posture rests
 * on something other than arithmetic.
 */
static void channel_random_bytes(uint8_t *out, size_t len) {
    for (size_t i = 0; i < len; i += sizeof(uint64_t)) {
        const uint64_t bits = get_rand_64();
        const size_t take = (len - i) < sizeof bits ? (len - i) : sizeof bits;
        memcpy(out + i, &bits, take);
    }
}

/*
 * The board's own key pair, read back or drawn and written once.
 *
 * Its sector is beside ADDR_CONFIG and part of neither the configuration nor
 * the running image, so a wipe, a firmware update and the peer propagation
 * that copies board A's image onto board B (#91) all leave it alone. An
 * identity inside the image would give both boards one identity —
 * src/include/flash_layout.h asserts the placement, tests/flash_layout_test.c
 * gates it.
 *
 * Generating costs ~134 ms (#110) and happens at boot, before the scheduler
 * starts, which is the only place on this board where that is free.
 */
static void channel_load_identity(device_t *state) {
    uint8_t private_key[DH_P256_PRIVATE_SIZE];

    if (load_identity(private_key) && dh_pair_set_identity(&channel.pair, private_key))
        return;

    /*
     * Bounded, because dh_pair_set_identity refuses 32 bytes that are not a
     * scalar in [1, n-1] and the honest answer to that is to draw again — but
     * a loop with no bound would spin for good against an entropy source that
     * had failed, and a board that boots without an identity can still be
     * flashed. Random bytes fail this about once in 2^32 draws.
     */
    for (int attempt = 0; attempt < 8; attempt++) {
        channel_random_bytes(private_key, sizeof private_key);
        if (!dh_pair_set_identity(&channel.pair, private_key))
            continue;
        save_identity(state, private_key);
        return;
    }
}

/* The registration this board holds, as stored. */
static void channel_load_registration(const device_t *state) {
    if (state->config.channel_paired)
        dh_pair_set_registration(&channel.pair, state->config.channel_helper_key_id,
                                 state->config.channel_shared_secret);
    else
        dh_pair_clear_registration(&channel.pair);
}

/*
 * Everything that belongs to one connection: the session, the frame reader,
 * the relay and whatever was owed to a helper that is no longer there.
 * The identity and the registration are deliberately *not* here — see
 * channel_reset_link's caller.
 */
static void channel_reset_link(void) {
    dh_session_drop(&channel.session);
    dh_frame_reader_init(&channel.reader);
    dh_relay_tx_reset(&channel.relay_tx);
    dh_relay_rx_reset(&channel.relay_rx);

    channel.report_head = 0;
    channel.report_used = 0;
    dh_inq_reset(&channel.inbound);

    /* Reset, never init: what is queued belonged to the link that just went,
       and the drop totals did not (#142). */

    critical_section_enter_blocking(&channel.out_lock);
    channel.cursor_query_origin = CURSOR_QUERY_NONE;
    channel.cursor_query_id = 0;
    dh_outq_reset(&channel.out);
    critical_section_exit(&channel.out_lock);
}

void channel_init(device_t *state) {
    if (!channel.locked) {
        critical_section_init(&channel.out_lock);
        channel.locked = true;
    }

    dh_session_init(&channel.session, CHANNEL_BUILD_TYPE);
    dh_pair_init(&channel.pair);

    /*
     * The one place the queues are built, and so the one place the drop totals
     * start from zero. Everything after this resets them and keeps the counts,
     * which is what makes "since this boot" true of all seven rather than four
     * (#142) — this runs once, from setup.c, at boot.
     *
     * dh_relay_rx_init is also the only call that installs the reassembly
     * buffer, which is why it belongs here and not in the reset beside it.
     */
    critical_section_enter_blocking(&channel.out_lock);
    dh_outq_init(&channel.out);
    critical_section_exit(&channel.out_lock);
    dh_relay_tx_init(&channel.relay_tx);
    dh_relay_rx_init(&channel.relay_rx, channel.relay_rx_buf, sizeof channel.relay_rx_buf);
    dh_inq_init(&channel.inbound);

    channel_load_identity(state);
    channel_load_registration(state);
    channel_reset_link();
}

/*
 * The configuration was wiped, and the registration lived in it. Wiping is how
 * a user revokes a paired machine, so the registration has to go with the
 * flash sector rather than staying live in RAM until the next power-up — which
 * is what it did until #75, leaving the board happily authenticating a helper
 * against a secret that no longer existed anywhere, with nothing said about it
 * on any surface.
 *
 * The board's identity is not the wipe's to take. A wipe that changed who the
 * board is would make every helper report "this board changed" on a routine
 * action, which is a false alarm and a worse one than no alarm.
 *
 * Deferred to channel_task rather than done here: both wipe paths reach this
 * from core 1, and the session is core 0's. See config_wiped.
 */
void channel_config_wiped(void) {
    channel.config_wiped = true;
}

/*
 * The USB interface went away — a re-enumeration, a suspend, or the config
 * mode round trip. The connection goes with it, but an open pairing window
 * does not: the user pressed the chord and has a minute, and a bus reset in
 * the middle of it is not their doing and must not silently cost them the
 * window.
 */
void channel_link_lost(void) {
    channel_reset_link();
}

void channel_open_pairing_window(void) {
    /*
     * Open the window, and nothing else. v1 rotated the device secret on every
     * chord press, because a bearer token that had leaked stayed valid until
     * the configuration was wiped — and ADR-0008 recorded the sting in that:
     * where the channel is not exclusive, rotating *re-issued* the pairing to
     * whatever was listening. Nothing secret crosses now, so there is nothing
     * to rotate, and a chord press nobody pairs against leaves the existing
     * registration exactly as it was. An accidental press costs the user
     * nothing.
     */
    dh_pair_open_window(&channel.pair, channel_now_ms());

    /*
     * Clearing the session here is defensive, not an eviction anyone hears
     * about: the only caller is setup.c, on the normal-mode boot after the
     * chord's reboot, where channel_init has just cleared it anyway and no
     * helper has said hello. The helper re-pairs silently inside the window
     * it is standing in, having learned of the reboot from the USB
     * re-enumeration — a louder signal than any frame could be.
     */
    dh_session_drop(&channel.session);
}

/* Consumed once, on the normal-mode boot after a config chord. */
bool channel_pairing_window_owed(void) {
    if (watchdog_hw->scratch[3] != MAGIC_WORD_PAIR)
        return false;

    watchdog_hw->scratch[3] = 0;
    return true;
}

bool channel_helper_present(void) {
    return channel.session.present;
}

cursor_query_result_t channel_query_cursor(uint8_t output, uint8_t query_id) {
    if (output != BOARD_ROLE) {
        uart_packet_t packet = {
            .type = CURSOR_QUERY_MSG,
            .data = {output, query_id},
        };
        return queue_uart_packet(&packet, &global_state) ? CURSOR_QUERY_SENT
                                                         : CURSOR_QUERY_RETRY;
    }
    if (!channel_helper_present())
        return CURSOR_QUERY_UNAVAILABLE;
    critical_section_enter_blocking(&channel.out_lock);
    const bool accepted = channel.cursor_query_origin == CURSOR_QUERY_NONE;
    if (accepted) {
        channel.cursor_query_origin = CURSOR_QUERY_LOCAL;
        channel.cursor_query_id = query_id;
    }
    critical_section_exit(&channel.out_lock);
    return accepted ? CURSOR_QUERY_SENT : CURSOR_QUERY_RETRY;
}

void handle_cursor_query_msg(uart_packet_t *packet, device_t *state) {
    if (!channel_helper_present()) {
        uart_packet_t unavailable = {
            .type = CURSOR_QUERY_UNAVAILABLE_MSG,
            .data = {(uint8_t)BOARD_ROLE, packet->data[1]},
        };
        (void)queue_uart_packet(&unavailable, state);
        return;
    }
    critical_section_enter_blocking(&channel.out_lock);
    const bool accepted = channel.cursor_query_origin == CURSOR_QUERY_NONE;
    if (accepted) {
        channel.cursor_query_origin = CURSOR_QUERY_PEER;
        channel.cursor_query_id = packet->data[1];
    }
    critical_section_exit(&channel.out_lock);
    if (!accepted) {
        uart_packet_t unavailable = {
            .type = CURSOR_QUERY_UNAVAILABLE_MSG,
            .data = {(uint8_t)BOARD_ROLE, packet->data[1]},
        };
        (void)queue_uart_packet(&unavailable, state);
    }
}

void channel_place_cursor(uint8_t output, uint8_t screen, uint8_t chain, uint8_t border,
                          uint16_t position) {
    if (output != BOARD_ROLE) {
        uart_packet_t packet = {
            .type = CURSOR_PLACE_MSG,
            .data = {screen, chain, border},
        };
        packet.data16[2] = position;
        (void)queue_uart_packet(&packet, &global_state);
        return;
    }
    if (!channel_helper_present())
        return;

    const dh_place place = {
        .chain_index = screen,
        .chain_direction = chain,
        .border_direction = border,
        .entry_position = position,
    };
    uint8_t body[DH_PLACE_BODY_SIZE];
    if (!dh_place_encode(&place, body, sizeof body))
        return;
    const uint8_t query[] = {0};
    if (channel_emit_placement_body(DH_MSG_PLACE, body, sizeof body))
        (void)channel_emit_placement_body(DH_MSG_POS_QUERY, query, sizeof query);
}

bool channel_place_cursor_correlated(uint8_t output, uint8_t screen, uint8_t chain,
                                     uint8_t border, uint16_t position, uint8_t query_id) {
    if (query_id == 0)
        return false;
    if (output != BOARD_ROLE) {
        uart_packet_t packet = {
            .type = CURSOR_PLACE_MSG,
            .data = {screen, chain, border},
        };
        packet.data16[2] = position;
        packet.data[7] = query_id;
        return queue_uart_packet(&packet, &global_state);
    }
    if (!channel_helper_present())
        return false;

    const dh_place place = {
        .chain_index = screen,
        .chain_direction = chain,
        .border_direction = border,
        .entry_position = position,
    };
    uint8_t body[DH_PLACE_BODY_SIZE];
    return dh_place_encode(&place, body, sizeof body) &&
           channel_emit_placement_query(body, query_id);
}

/* Hand a whole frame to this board's helper. */
static bool channel_queue_frame(const uint8_t *frame, size_t len) {
    const uint32_t now = channel_now_ms();

    critical_section_enter_blocking(&channel.out_lock);
    const bool queued = dh_outq_offer(&channel.out, frame, len) == DH_OUTQ_OK;
    if (queued) {
        /*
         * Anything accepted for this helper already proves the board is alive
         * and holding a session, so it feeds the idle timer and the beat stays
         * out of the way (ADR-0004). That gating is what keeps a sustained
         * transfer from starving the beat out of this queue and looking, to
         * the helper, exactly like a board that stopped answering.
         *
         * Everything that reaches here now runs on core 0 — the pump, and the
         * relayed frame core 1 hands over rather than queues itself — so being
         * inside the out lock is incidental. The lock is still what the queue
         * needs, because dh_outq_advance runs against the transport.
         */
        dh_session_note_sent(&channel.session, now);
    }
    critical_section_exit(&channel.out_lock);

    /*
     * A refusal is still data loss with no retransmit beneath it, but it now
     * takes a burst deeper than the queue to cause one (#69, ADR-0005). What
     * is left is sustained overrun, which no bounded queue can absorb and
     * which the credit window owns end to end between the helpers — the board
     * may not enforce it, because that means reading a payload (ADR-0003).
     *
     * Counted rather than silent, and what acts on the gap depends on what was
     * lost: a chunk is re-requested by the receiving helper's chunk accounting,
     * while an offer or a done has no retransmit behind it and costs the whole
     * transfer, out to that helper's timeout. That asymmetry is why a queued
     * slot is sized to hold either of them (dh_outq.h).
     */
    return dh_txq_track(&channel.tx, queued);
}

static bool channel_emit_placement_body(uint8_t type, const uint8_t *body, size_t body_len) {
    if (body_len > DH_PLACE_BODY_SIZE)
        return false;
    uint8_t frame_bytes[DH_FRAME_HEADER_SIZE + DH_FRAME_AUTH_PREFIX_SIZE + DH_PLACE_BODY_SIZE];
    size_t frame_len = 0;
    const dh_frame_view frame = {
        .hdr = {.type = type, .flags = 0, .len = (uint16_t)body_len},
        .payload = body,
    };
    return dh_session_emit_relayed(&channel.session, &frame, frame_bytes, sizeof frame_bytes,
                                   &frame_len) == DH_FRAME_OK &&
           channel_queue_frame(frame_bytes, frame_len);
}

/* PLACE and its correlated POS_QUERY are one transaction on the channel. Do
   not queue PLACE unless the priority band can accept both frames: falling
   back after sending only the placement would recreate the cross-endpoint race
   this transaction exists to remove. */
static bool channel_emit_placement_query(const uint8_t *place_body, uint8_t query_id) {
    uint8_t place_frame[DH_FRAME_HEADER_SIZE + DH_FRAME_AUTH_PREFIX_SIZE + DH_PLACE_BODY_SIZE];
    uint8_t query_frame[DH_FRAME_HEADER_SIZE + DH_FRAME_AUTH_PREFIX_SIZE + DH_POS_QUERY_BODY_SIZE];
    size_t place_len = 0;
    size_t query_len = 0;
    const uint8_t query_body[] = {query_id};
    const dh_frame_view place = {
        .hdr = {.type = DH_MSG_PLACE, .flags = 0, .len = DH_PLACE_BODY_SIZE},
        .payload = place_body,
    };
    const dh_frame_view query = {
        .hdr = {.type = DH_MSG_POS_QUERY, .flags = 0, .len = DH_POS_QUERY_BODY_SIZE},
        .payload = query_body,
    };

    critical_section_enter_blocking(&channel.out_lock);
    bool queued = false;
    if (dh_session_emit_relayed(&channel.session, &place, place_frame, sizeof place_frame,
                                &place_len) == DH_FRAME_OK &&
        dh_session_emit_relayed(&channel.session, &query, query_frame, sizeof query_frame,
                                &query_len) == DH_FRAME_OK &&
        dh_outq_offer_pair(&channel.out, place_frame, place_len,
                           query_frame, query_len) == DH_OUTQ_OK) {
        dh_session_note_sent(&channel.session, channel_now_ms());
        queued = true;
    }
    critical_section_exit(&channel.out_lock);
    return dh_txq_track(&channel.tx, queued);
}

/*
 * End the session and tell the helper why — best effort. A refused queue
 * leaves it to notice for itself, which is precisely what its own timeout is
 * for: this is an optimisation over that timeout, never a substitute for it.
 */
static void channel_end_session(uint8_t reason) {
    uint8_t frame[DH_SESSION_REPLY_MAX];
    size_t len = 0;
    /* Zero for every reason but a liveness timeout, which is the one that is
       asserting something about a clock and the one #107 needs to read. */
    if (dh_session_end(&channel.session, reason, 0, frame, sizeof frame, &len) == DH_FRAME_OK &&
        len > 0)
        (void)channel_queue_frame(frame, len);
}

/*
 * One HID OUT report, copied and nothing more.
 *
 * Every decision this channel makes used to happen here, inside a TinyUSB
 * callback on core 0's main stack — 2 KB inside a 4 KB SCRATCH_Y whose
 * neighbour is core 1's stack. v2 puts cryptography behind those decisions: an
 * ECDH at pairing wants ~700 bytes of stack and 133 ms of wall clock (#110),
 * and every frame carries a tag to verify. Neither belongs at the bottom of a
 * callback that TinyUSB has already spent stack reaching. So the report is
 * queued and channel_task does the work, on the shallow stack of the
 * scheduler's own loop.
 */
void channel_receive_report(const uint8_t *buffer, uint16_t bufsize) {
    if (bufsize == 0)
        return;

    /* Before the backlog check, so this is what arrived rather than what fitted. */
    if (channel.reports_in != UINT32_MAX)
        channel.reports_in++;

    if (channel.report_used >= CHANNEL_REPORT_BACKLOG) {
        /* Counted, never silent (#43). A lost report loses a frame, which the
           helper's own machinery re-requests or times out on. */
        channel.reports_dropped++;
        return;
    }

    const uint8_t slot = (uint8_t)((channel.report_head + channel.report_used) %
                                   CHANNEL_REPORT_BACKLOG);
    const uint16_t take = bufsize < CHANNEL_REPORT_SIZE ? bufsize : CHANNEL_REPORT_SIZE;
    memcpy(channel.reports[slot], buffer, take);
    channel.report_len[slot] = take;
    channel.report_used++;
}

/*
 * A bulk frame the helper authenticated, on its way to the peer board.
 *
 * What crosses the inter-board link is the frame **without** its
 * authentication prefix: the tag is per hop, board A's means nothing to
 * board B, and board B writes its own before emitting it. Sending the dead
 * prefix would cost 24 bytes a frame on the link ADR-0002 measured as the wall
 * for no reader anywhere.
 *
 * The shortened frame is built in place, over the last four bytes of the tag
 * that has just been verified and will never be read again — so a 1 KB chunk
 * is relayed without a second buffer to hold it in. The reader's own header at
 * the front of its buffer is untouched, which is what dh_frame_reader_push
 * uses on the next call to release the frame it returned.
 */
static void channel_relay_to_peer(const dh_frame_view *frame, const uint8_t *body,
                                  size_t body_len) {
    uint8_t *header = (uint8_t *)body - DH_FRAME_HEADER_SIZE;
    header[0] = frame->hdr.type;
    header[1] = frame->hdr.flags;
    header[2] = (uint8_t)(body_len & 0xFFu);
    header[3] = (uint8_t)(body_len >> 8);

    /*
     * A refusal means the relay's queue is full, not merely that the previous
     * frame is still fragmenting — that burst is what the queue absorbs now
     * (#69, ADR-0005). Nothing here can hold the frame if it is refused, since
     * the reader releases it on the next push, so it is counted rather than
     * silently dropped (#43). Making the helper wait instead is the credit
     * window's job, and that window is end to end between the helpers: the
     * board may not enforce it without reading a payload (ADR-0003).
     */
    const dh_relay_result offered =
        dh_relay_tx_offer(&channel.relay_tx, header, DH_FRAME_HEADER_SIZE + body_len);
    (void)dh_txq_track(&channel.tx, offered == DH_RELAY_OK);
}

/* One decoded frame from this board's helper. */
static void channel_on_frame(device_t *state, const dh_frame_view *frame, uint32_t now) {
    /*
     * The whole routing decision, on the type byte alone: bulk is relayed to
     * the peer helper opaquely, everything below is addressed to this firmware
     * and is never forwarded. The payload is not read on either path.
     */
    if (frame->hdr.type >= DH_MSG_PLACE && frame->hdr.type <= DH_MSG_POS_RESPONSE) {
        const uint8_t *body = NULL;
        size_t body_len = 0;
        if (dh_session_authenticate(&channel.session, frame, now, &body, &body_len) != DH_AUTH_OK)
            return;
        if (frame->hdr.type == DH_MSG_POS_RESPONSE) {
            dh_position position;
            /* A response can arrive after the user has already crossed back.
               It describes this board's output, so applying it while the peer
               is active would rewind the global pointer to stale coordinates. */
            if (state->active_output != BOARD_ROLE ||
                !dh_position_decode(body, body_len, &position) || position.chain_index == 0 ||
                position.chain_index > state->config.output[BOARD_ROLE].screen_count)
                return;
            const int16_t pointer_x = (int16_t)(
                ((uint32_t)position.x * MAX_SCREEN_COORD + 32767u) / DH_SEAM_POSITION_MAX);
            const int16_t pointer_y = (int16_t)(
                ((uint32_t)position.y * MAX_SCREEN_COORD + 32767u) / DH_SEAM_POSITION_MAX);
            const bool applied = apply_helper_cursor_position(
                state, BOARD_ROLE, position.chain_index, pointer_x, pointer_y,
                position.query_id);
            /* A nonzero query may have originated on the peer board. This
               board has no matching crossing state in that case, but it must
               still relay the correlated answer to the requester. */
            if (!applied && position.query_id == 0)
                return;
            uart_packet_t packet = {
                .type = CURSOR_POSITION_MSG,
                .data = {(uint8_t)BOARD_ROLE, position.chain_index},
            };
            packet.data16[1] = (uint16_t)pointer_x;
            packet.data16[2] = (uint16_t)pointer_y;
            packet.data[6] = position.query_id;
            (void)queue_uart_packet(&packet, state);
        }
        return;
    }

    if (dh_msg_is_bulk(frame->hdr.type)) {
        /*
         * Authorisation is per frame. v1 gated this on dh_session_may_relay —
         * one flag for the whole board — so any process could push bulk into a
         * session it never authenticated, which is the isolation breach #34
         * exists to prevent. A frame that does not carry a good tag under the
         * session key is neither acted on nor relayed, whatever else is going
         * on, and is counted towards the listener alert.
         */
        const uint8_t *body = NULL;
        size_t body_len = 0;
        if (dh_session_authenticate(&channel.session, frame, now, &body, &body_len) != DH_AUTH_OK)
            return;

        channel_relay_to_peer(frame, body, body_len);
        return;
    }

    /*
     * Sized from the largest frame the session layer can produce, which is a
     * 76-byte pair grant. Under v1 this was report-sized on the argument that
     * a *frame*-sized buffer (DH_FRAME_MAX_SIZE, 4100) would overrun the
     * stack; that argument survives and 80 bytes does not trouble it. What did
     * not survive is the claim about the largest reply: a v2 board built on 64
     * bytes takes DH_FRAME_ERR_BUFFER from the encoder and therefore never
     * answers a pairing request at all, with no error anywhere (#109).
     */
    uint8_t reply[DH_SESSION_REPLY_MAX];
    size_t reply_len = 0;

    const uint32_t registrations = channel.pair.registrations;
    const uint32_t sessions = channel.session.sessions;
    const dh_frame_result rc = dh_session_on_frame(&channel.session, &channel.pair, frame, now,
                                                  reply, sizeof reply, &reply_len);

    /*
     * A new session begins with an empty stream to this helper.
     *
     * The helper that just said hello reopened its handles to say it — every
     * teardown closes them — so its frame reader is starting from nothing.
     * What this queue still held belongs to the session that went, and both
     * halves of it break the new one: the unsent tail of a half-drained frame,
     * which a fresh reader takes for a header and follows into garbage, and
     * whole frames tagged under keys that no longer exist, which fail their
     * tag. Both drop the session as fast as it was made, and the board is then
     * mid-frame again on the next attempt — the flap #143's log ends on.
     *
     * Ahead of the ack, so the ack's first byte is the stream's first byte.
     * The refusal totals survive, as they do on every other reset (#142).
     */
    if (channel.session.sessions != sessions) {
        critical_section_enter_blocking(&channel.out_lock);
        dh_outq_reset(&channel.out);
        critical_section_exit(&channel.out_lock);
    }

    /* A registration is the one thing here that has to outlive a power cut. */
    if (channel.pair.registrations != registrations) {
        memcpy(state->config.channel_helper_key_id, channel.pair.helper_key_id,
               sizeof state->config.channel_helper_key_id);
        memcpy(state->config.channel_shared_secret, channel.pair.shared_secret,
               sizeof state->config.channel_shared_secret);
        state->config.channel_paired = 1;
        channel.registration_unsaved = true;
    }

    if (rc == DH_FRAME_OK && reply_len > 0)
        (void)channel_queue_frame(reply, reply_len);
}

/*
 * Drain the reports the USB callback left, decoding frames out of the stream.
 *
 * `now` is the caller's, never a fresh read. This read the clock for itself
 * once, and stamped the session's liveness deadline with a value *later* than
 * the one channel_task then judged that deadline against — so a millisecond
 * turning over between the two reads, while a frame happened to arrive, left
 * the stamp one ahead of the clock and the difference wrapped. The board then
 * evicted a helper it had heard from that instant (#107).
 */
static void channel_drain_reports(device_t *state, uint32_t now) {

    while (channel.report_used > 0) {
        const uint8_t slot = channel.report_head;
        const uint8_t *buffer = channel.reports[slot];
        const uint16_t bufsize = channel.report_len[slot];
        channel.report_head = (uint8_t)((channel.report_head + 1u) % CHANNEL_REPORT_BACKLOG);
        channel.report_used--;

        size_t offset = 0;
        while (offset < bufsize) {
            dh_frame_view frame;
            size_t consumed = 0;
            const dh_frame_result rc = dh_frame_reader_push(
                &channel.reader, buffer + offset, bufsize - offset, &consumed, &frame);

            if (rc != DH_FRAME_OK && rc != DH_FRAME_AGAIN) {
                /* A protocol error drops the session: the stream is no longer
                   trustworthy and the helper reconnects (docs/protocol.md). It
                   is told so rather than left to time out, because until it
                   finds out it goes on writing into a reader it has
                   desynchronised — and this is the one path where the helper is
                   the thing in the wrong and could stop. */
                channel_end_session(DH_SESSION_END_PROTOCOL_ERROR);
                dh_frame_reader_init(&channel.reader);
                return;
            }

            offset += consumed;

            if (rc == DH_FRAME_OK)
                channel_on_frame(state, &frame, now);
            else if (consumed == 0)
                break; /* nothing more to take from this report */
        }
    }
}

/* One inter-board packet of relayed frame, arriving from the peer board.
   Runs on core 1. */
void handle_channel_relay_msg(uart_packet_t *packet, device_t *state) {
    (void)state;

    dh_relay_packet relayed = {
        .kind = (packet->type == CHANNEL_START_MSG) ? DH_RELAY_PKT_START : DH_RELAY_PKT_DATA,
        .len = DH_RELAY_PAYLOAD,
    };
    memcpy(relayed.data, packet->data, DH_RELAY_PAYLOAD);

    dh_frame_view frame;
    if (dh_relay_rx_push(&channel.relay_rx, &relayed, &frame) != DH_RELAY_OK)
        return; /* incomplete, or a loss the reassembler has already counted */

    /* Refused when core 0 is more than a pump batch behind, or when the frame
       is longer than any a transfer completes with. Counted either way, and
       the receiving helper re-requests a chunk — but not an offer, a done or a
       credit, which is why the ring is sized to make this rare (dh_inq.h). */
    const size_t total = DH_FRAME_HEADER_SIZE + frame.hdr.len;
    if (!dh_inq_stage(&channel.inbound, frame.payload - DH_FRAME_HEADER_SIZE, (uint16_t)total))
        return;

    /* The published slot is what core 0 reads, so the bytes must be visible
       before it moves. Cortex-M0+ retires in order, but the compiler is under
       no such obligation. */
    __dmb();
    dh_inq_publish(&channel.inbound);
}

/*
 * Whatever the peer board handed over, tagged for this board's helper.
 *
 * Drained to exhaustion rather than one frame per pass. One per pass caps this
 * seam at 1000 frames a second, which sounds ample and is not: the burst that
 * overruns it is short frames — a relayed CLIP_CREDIT is ten bytes — and a
 * batch of those crosses the link inside a fraction of one pass. The ring
 * parks them; taking only one of them per pass would simply move where they
 * are lost (#139).
 *
 * The loop stops on a refused enqueue instead of running the ring dry into a
 * full queue. The frames behind it stay parked and go out on a later pass,
 * which is the back-pressure this seam otherwise has none of.
 *
 * What this costs per pass is a tag per frame, and that cost is not yet
 * measured (#115). It is bounded by DH_INQ_DEPTH and, in the traffic that
 * fills the ring, small: the ring fills with short frames, because a full
 * chunk takes about 4 ms to cross the link and is drained long before a second
 * one lands. A ring full of full-size chunks is not reachable at the rate the
 * link delivers them — if #115 finds otherwise, this is the loop to bound.
 */
static void channel_pump_inbound(void) {
    const uint8_t *at = NULL;
    uint16_t len = 0;

    while (dh_inq_peek(&channel.inbound, &at, &len)) {
        dh_frame_view frame;
        size_t consumed = 0;
        size_t tagged_len = 0;

        const bool tagged =
            dh_frame_decode(at, len, &frame, &consumed) == DH_FRAME_OK &&
            /*
             * The same gate as the outbound direction, and for the sharper
             * reason: without it, a local process that holds this board's
             * channel and never authenticates is still handed everything the
             * *other* computer's paired helper sends. That is precisely the
             * cross-machine path #34 exists to close, and it is not closed by
             * refusing to relay outward alone. dh_session_emit_relayed refuses
             * outright when there is no session, because without one there is
             * no key to tag under either.
             */
            dh_session_emit_relayed(&channel.session, &frame, channel.tagged,
                                    sizeof channel.tagged, &tagged_len) == DH_FRAME_OK;

        /* Read before the slot goes back to core 1. */
        __dmb();
        dh_inq_release(&channel.inbound);

        /* A frame the queue refused is lost and counted there, as it always
           was. What is new is that the rest of the ring is not lost with it. */
        if (tagged && !channel_queue_frame(channel.tagged, tagged_len))
            break;
    }
}

/* Drain what the relay owes into the shared inter-board queue. The burst cap
   inside the relay is what keeps a chunk's packets from filling the queue
   ahead of keyboard and mouse traffic. */
static void channel_pump_relay(void) {
    dh_relay_tx_yield(&channel.relay_tx);

    dh_relay_packet packet;
    while (dh_relay_tx_peek(&channel.relay_tx, &packet)) {
        const enum packet_type_e type =
            (packet.kind == DH_RELAY_PKT_START) ? CHANNEL_START_MSG : CHANNEL_DATA_MSG;

        /* A refused enqueue leaves the packet owed rather than lost: a frame
           missing one data packet would corrupt everything after it. */
        if (!queue_packet(packet.data, type, DH_RELAY_PAYLOAD))
            break;

        dh_relay_tx_commit(&channel.relay_tx);
    }
}

/* One report's worth of whatever is owed to this board's helper. */
static void channel_pump_out(void) {
    if (global_state.config_mode_active)
        return;

    /* The channel occupies the vendor interface slot in normal mode, with no
       report ID: a report is exactly one packet the framing layer owns. */
    if (!tud_hid_n_ready(ITF_NUM_HID_VENDOR))
        return;

    uint8_t report[CHANNEL_REPORT_SIZE];
    dh_outq_view owed;
    uint16_t take = 0;

    critical_section_enter_blocking(&channel.out_lock);
    if (dh_outq_peek(&channel.out, &owed)) {
        take = owed.remaining < CHANNEL_REPORT_SIZE ? owed.remaining : CHANNEL_REPORT_SIZE;

        /* Pad the tail: a report is a fixed 64 bytes with no length of its own,
           and DH_FRAME_PAD is what a decoder skips between frames. */
        memset(report, DH_FRAME_PAD, sizeof report);
        memcpy(report, owed.at, take);
    }
    critical_section_exit(&channel.out_lock);

    if (take == 0)
        return;

    /* The lock is released across this call rather than held into TinyUSB, so
       the other core can still queue a frame here. Advancing the band the peek
       named — not whatever is owed by the time we return — is what makes that
       gap safe; a frame that arrived meanwhile simply waits its turn. */
    if (!tud_hid_n_report(ITF_NUM_HID_VENDOR, 0, report, CHANNEL_REPORT_SIZE))
        return; /* refused: the bytes stay owed rather than being lost */

    critical_section_enter_blocking(&channel.out_lock);
    dh_outq_advance(&channel.out, &owed, take);
    critical_section_exit(&channel.out_lock);
}

void channel_task(device_t *state) {
    const uint32_t now = channel_now_ms();

    /*
     * A wipe, applied where the session is safe to rewrite. Ahead of everything
     * else so a session about to end is not beaten at first, and the helper is
     * told rather than left to its timeout: it is authenticated against a
     * registration that has just stopped existing, and the sooner it reconnects
     * the sooner it can say so and ask to be paired again.
     *
     * The identity survives, deliberately. #75's shape holds: this takes effect
     * on the next tick, with no power cycle.
     */
    if (channel.config_wiped) {
        channel.config_wiped = false;
        channel_end_session(DH_SESSION_END_UNPAIRED);
        dh_pair_clear_registration(&channel.pair);
        channel.registration_unsaved = false;
    }

    /*
     * The board nonce the next hello ack will carry. Drawn only when the last
     * one has been spent, rather than on every tick: get_rand_64 is not free,
     * and a nonce is needed about as often as a session begins.
     */
    if (dh_session_needs_nonce(&channel.session)) {
        uint8_t nonce[DH_NONCE_SIZE];
        channel_random_bytes(nonce, sizeof nonce);
        dh_session_stage_nonce(&channel.session, nonce);
    }

    channel_drain_reports(state, now);

    critical_section_enter_blocking(&channel.out_lock);
    const int query_origin = channel.cursor_query_origin;
    const uint8_t query_id = channel.cursor_query_id;
    critical_section_exit(&channel.out_lock);
    bool query_finished = false;
    if (query_origin != CURSOR_QUERY_NONE) {
        if (!channel_helper_present()) {
            if (query_origin == CURSOR_QUERY_PEER) {
                uart_packet_t unavailable = {
                    .type = CURSOR_QUERY_UNAVAILABLE_MSG,
                    .data = {(uint8_t)BOARD_ROLE, query_id},
                };
                query_finished = queue_uart_packet(&unavailable, state);
            } else {
                query_finished = true;
            }
        } else {
            const uint8_t body[] = {query_id};
            query_finished = channel_emit_placement_body(DH_MSG_POS_QUERY, body, sizeof body);
        }
        if (query_finished) {
            critical_section_enter_blocking(&channel.out_lock);
            if (channel.cursor_query_origin == query_origin &&
                channel.cursor_query_id == query_id)
                channel.cursor_query_origin = CURSOR_QUERY_NONE;
            critical_section_exit(&channel.out_lock);
        }
    }

    /* Written after the grant is already in the outbound queue, because a
       grant the helper never receives is a pairing neither end holds — and
       flash is slow enough to be worth keeping off the path that queues it. */
    if (channel.registration_unsaved) {
        channel.registration_unsaved = false;
        save_config(state);
    }

    channel_pump_inbound();

    /*
     * What this board has dropped, published twice over.
     *
     * To the config API first (#52), where it was the only reader — and could
     * not be a useful one: the page is reachable only in config mode, config
     * mode is entered by rebooting, and these counters live in plain RAM. So
     * every reading was taken on a board that had just zeroed them, and three
     * sittings on #132 read that row of zeros as evidence the seams were
     * clean. Kept because the page is still the only reader on a board with no
     * helper paired, and because a since-boot number is honest as long as
     * whoever reads it knows the boot just happened.
     *
     * Then to the session, which is where the number is worth something
     * (#133): the helper is attached while the fault is happening, so it can
     * read these live, at the moment of a stall, with no reboot at all.
     *
     * Published every pass rather than on change: these are counters, and a
     * reader of one is asking what the total is now. The session decides for
     * itself whether a fresh reading is worth a frame.
     */
    state->_channel_reports_dropped = channel.reports_dropped;
    state->_channel_inbound_dropped = channel.inbound.dropped;
    state->_channel_outq_refused = channel.out.refused;
    state->_channel_relay_dropped = channel.tx.dropped;
    state->_channel_relay_orphans = channel.relay_rx.orphans;
    state->_channel_relay_truncated = channel.relay_rx.truncated;
    state->_channel_relay_refused = channel.relay_tx.q.refused;

    const dh_device_drops drops = {
        .reports = channel.reports_dropped,
        .inbound = channel.inbound.dropped,
        .outq = channel.out.refused,
        .unsent = channel.tx.dropped,
        .orphans = channel.relay_rx.orphans,
        .truncated = channel.relay_rx.truncated,
        .relay_q = channel.relay_tx.q.refused,
        .reports_in = channel.reports_in,
        .frames_in = channel.session.frames_in,
        .frames_refused = channel.session.frames_refused,
        .outq_priority = channel.out.refused_priority,
        .outq_bad_header = channel.out.refused_bad_header,
    };
    dh_session_set_drops(&channel.session, &drops);

    /*
     * The clipboard's two direction toggles, as the two verbs this board's own
     * helper acts on (#52). Set every pass rather than watched for changes:
     * the session sends a frame only when the value it holds is no longer this
     * one, so a config page write takes effect on a live session without
     * anything here having to notice that a setting moved.
     */
    dh_session_set_clip_policy(&channel.session,
                               dh_clip_policy_for(state->board_role,
                                                  state->config.clip_block_a_to_b != 0,
                                                  state->config.clip_block_b_to_a != 0),
                               state->config.clip_cap_mb);

    /*
     * Whichever the session owes its helper: the clipboard policy a fresh or
     * changed setting owes it, a listener alert that has been waiting for a
     * session to tell, a fresh reading of the drop totals above, the beat that
     * fills an idle direction, or the announcement that a silent helper has
     * just been evicted. Never more than one, and nothing at all on the
     * ordinary tick.
     */
    uint8_t owed[DH_SESSION_REPLY_MAX];
    size_t owed_len = 0;
    if (dh_session_tick(&channel.session, now, owed, sizeof owed, &owed_len) == DH_FRAME_OK &&
        owed_len > 0 && channel_queue_frame(owed, owed_len)) {
        /* The listener alert and the clipboard policy both care, and both for
           the same reason: the queue's priority band holds one frame, and the
           tick that first has a session to tell is the one right after the
           HELLO_ACK went into it. A refused alert marked sent would be a
           measurement destroyed; a refused policy marked sent would leave a
           helper acting on a toggle the user has changed, with nothing
           following to correct it. owed[0] is the frame's type byte. */
        dh_session_note_owed_sent(&channel.session, owed[0]);
    }

    dh_pair_tick(&channel.pair, now);
    channel_pump_relay();
    channel_pump_out();
}
