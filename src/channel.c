/*
 * The device's end of the helper channel (#45) and its relay across the
 * inter-board link (#47). See include/channel.h.
 */

#include "main.h"

#include <pico/critical_section.h>
#include <pico/rand.h>

#include "dh_outq.h"
#include "dh_pair.h"
#include "dh_relay.h"
#include "dh_session.h"
#include "dh_txq.h"

#ifdef DH_DEV_NO_AUTH
#define CHANNEL_BUILD_TYPE DH_BUILD_DEVELOPMENT
#else
#define CHANNEL_BUILD_TYPE DH_BUILD_RELEASE
#endif

static struct {
    dh_session session;
    dh_pair pair;
    dh_frame_reader reader;
    dh_txq_stats tx; /* replies lost to a busy endpoint, never silently */

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
     * The outbound queue is written from both cores: core 0 in the USB
     * callback and the pump, core 1 in the inter-board packet handler. An
     * interleaving there splices two frames into one byte stream, which the
     * helper reads as a framing error and drops the session over.
     */
    critical_section_t out_lock;
    bool locked;
} channel;

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
 * A fresh secret. pico_rand seeds from ring-oscillator jitter, the bus
 * performance counters and the board id — a secret that is guessable is no
 * secret, and this one is never displayed or typed, so its only source of
 * unpredictability is here.
 */
static void channel_fresh_secret(uint8_t *out) {
    for (size_t i = 0; i < DH_PAIR_SECRET_LEN; i += sizeof(uint64_t)) {
        const uint64_t bits = get_rand_64();
        const size_t take = (DH_PAIR_SECRET_LEN - i) < sizeof bits ? (DH_PAIR_SECRET_LEN - i)
                                                                  : sizeof bits;
        memcpy(out + i, &bits, take);
    }
}

/*
 * Everything that belongs to one connection: the session, the frame reader,
 * the relay and whatever was owed to a helper that is no longer there.
 * Pairing is deliberately *not* here — see channel_reset_link's caller.
 */
static void channel_reset_link(void) {
    dh_session_drop(&channel.session);
    dh_frame_reader_init(&channel.reader);
    dh_relay_tx_init(&channel.relay_tx);
    dh_relay_rx_init(&channel.relay_rx, channel.relay_rx_buf, sizeof channel.relay_rx_buf);

    critical_section_enter_blocking(&channel.out_lock);
    dh_outq_init(&channel.out);
    critical_section_exit(&channel.out_lock);
}

void channel_init(void) {
    if (!channel.locked) {
        critical_section_init(&channel.out_lock);
        channel.locked = true;
    }

    dh_session_init(&channel.session, CHANNEL_BUILD_TYPE);

    /* The secret survives a restart because it lives in the configuration.
       A wiped configuration leaves none, and one chord press restores it. */
    dh_pair_init(&channel.pair,
                 global_state.config.channel_paired ? global_state.config.channel_secret : NULL);
    channel_reset_link();
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

void channel_open_pairing_window(device_t *state) {
    /*
     * Rotate, always (#46). A window that reissued the same secret would
     * leave a pairing that leaked to whatever was connected last time valid
     * for good, recoverable only by wiping every setting the user has. This
     * evicts the helper on *this* board, which then re-pairs silently inside
     * the window it is standing in.
     */
    uint8_t fresh[DH_PAIR_SECRET_LEN];
    channel_fresh_secret(fresh);

    dh_pair_open_window(&channel.pair, fresh, channel_now_ms());

    /*
     * Clearing the session here is defensive, not an eviction anyone hears
     * about: the only caller is setup.c, on the normal-mode boot after the
     * chord's reboot, where channel_init has just cleared it anyway and no
     * helper has said hello. The helper re-pairs silently inside the window
     * it is standing in, having learned of the reboot from the USB
     * re-enumeration — a louder signal than any frame could be.
     */
    dh_session_drop(&channel.session);

    memcpy(state->config.channel_secret, fresh, sizeof fresh);
    state->config.channel_paired = 1;
    save_config(state);
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


/* Hand a whole frame to this board's helper. */
static bool channel_queue_frame(const uint8_t *frame, size_t len) {
    const uint32_t now = channel_now_ms();

    critical_section_enter_blocking(&channel.out_lock);
    const bool queued = dh_outq_offer(&channel.out, frame, len) == DH_OUTQ_OK;
    if (queued) {
        /*
         * Anything accepted for this helper already proves the device is
         * alive and holding a session, so it feeds the idle timer and the
         * beat stays out of the way (ADR-0004). That gating is what keeps a
         * sustained transfer from starving the beat out of this queue and
         * looking, to the helper, exactly like a device that stopped
         * answering.
         *
         * This runs on core 1 as well as core 0, and the rest of the session
         * is touched unlocked on core 0 — so being inside the out lock here
         * is incidental (we already hold it) and guarantees nothing about
         * the struct as a whole. It does not need to: this writes one
         * aligned 32-bit word that only the beat's idle test reads, so a
         * race costs a beat emitted or skipped one tick early. Core 0 can
         * even evict between the two, leaving a dropped session carrying a
         * fresh timestamp, which the next tick discards on !present.
         */
        dh_session_note_sent(&channel.session, now);
    }
    critical_section_exit(&channel.out_lock);

    /*
     * A refusal is still data loss with no retransmit beneath it, but it now
     * takes a burst deeper than the queue to cause one (#69, ADR-0005). What
     * is left is sustained overrun, which no bounded queue can absorb and
     * which the credit window owns end to end between the helpers — the device
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

/*
 * End the session and tell the helper why — best effort. A refused queue
 * leaves it to notice for itself, which is precisely what its own timeout is
 * for: this is an optimisation over that timeout, never a substitute for it.
 */
static void channel_end_session(uint8_t reason) {
    uint8_t frame[DH_FRAME_HEADER_SIZE + DH_SESSION_END_LEN];
    size_t len = 0;
    if (dh_session_end(&channel.session, reason, frame, sizeof frame, &len) == DH_FRAME_OK &&
        len > 0)
        (void)channel_queue_frame(frame, len);
}

void channel_receive_report(const uint8_t *buffer, uint16_t bufsize) {
    const uint32_t now = channel_now_ms();
    size_t offset = 0;

    while (offset < bufsize) {
        dh_frame_view frame;
        size_t consumed = 0;
        const dh_frame_result rc =
            dh_frame_reader_push(&channel.reader, buffer + offset, bufsize - offset, &consumed,
                                 &frame);

        if (rc != DH_FRAME_OK && rc != DH_FRAME_AGAIN) {
            /* A protocol error drops the session: the stream is no longer
               trustworthy and the helper reconnects (docs/protocol.md). It is
               told so rather than left to time out, because until it finds
               out it goes on writing into a reader it has desynchronised —
               and this is the one path where the helper is the thing in the
               wrong and could stop. */
            channel_end_session(DH_SESSION_END_PROTOCOL_ERROR);
            dh_frame_reader_init(&channel.reader);
            return;
        }

        offset += consumed;

        if (rc == DH_FRAME_OK) {
            /*
             * The whole routing decision, on the type byte alone: bulk is
             * relayed to the peer helper opaquely, everything below is
             * addressed to this firmware and is never forwarded. The payload
             * is not read on either path.
             */
            /* Nothing is relayed for an unauthenticated peer (#34). */
            if (dh_msg_is_bulk(frame.hdr.type)) {
                if (!dh_session_may_relay(&channel.session))
                    continue;

                /*
                 * Bulk is relayed opaquely and never reaches the session
                 * layer, so its liveness has to be noted here or it counts
                 * for nothing. It has to count: the helper suppresses its
                 * own beat while it has real traffic to send, so a device
                 * that only credited beats would evict a helper in the
                 * middle of a clipboard transfer — the same starvation the
                 * device's beat is idle-gated to avoid, in the other
                 * direction (ADR-0004).
                 */
                dh_session_note_received(&channel.session, now);

                /*
                 * A refusal means the relay's queue is full, not merely that
                 * the previous frame is still fragmenting — that burst is what
                 * the queue absorbs now (#69, ADR-0005). Nothing here can hold
                 * the frame if it is refused, since the reader releases it on
                 * the next push, so it is counted rather than silently dropped
                 * (#43). Making the helper wait instead is the credit window's
                 * job, and that window is end to end between the helpers: the
                 * device may not enforce it without reading a payload
                 * (ADR-0003).
                 */
                const dh_relay_result offered =
                    dh_relay_tx_offer(&channel.relay_tx, frame.payload - DH_FRAME_HEADER_SIZE,
                                      DH_FRAME_HEADER_SIZE + frame.hdr.len);
                (void)dh_txq_track(&channel.tx, offered == DH_RELAY_OK);
                continue;
            }

            /*
             * Report-sized, deliberately: this runs on core 0's main stack
             * inside a USB callback, and that stack is 2 KB inside a 4 KB
             * SCRATCH_Y whose neighbour is core 1's stack. A frame-sized
             * buffer here overruns both. The largest reply this path can
             * produce is a 20-byte pair grant.
             */
            uint8_t reply[CHANNEL_REPORT_SIZE];
            size_t reply_len = 0;
            if (dh_session_on_frame(&channel.session, &channel.pair, &frame, now, reply,
                                    sizeof reply, &reply_len) == DH_FRAME_OK &&
                reply_len > 0)
                (void)channel_queue_frame(reply, reply_len);
        } else if (consumed == 0) {
            break; /* nothing more to take from this report */
        }
    }
}

/* One inter-board packet of relayed frame, arriving from the peer board. */
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

    /*
     * The same gate as the outbound direction, and for the sharper reason:
     * without it, a local process that holds this board's channel and never
     * authenticates is still handed everything the *other* computer's paired
     * helper sends. That is precisely the cross-machine path #34 exists to
     * close, and it is not closed by refusing to relay outward alone.
     */
    if (!dh_session_may_relay(&channel.session))
        return;

    (void)channel_queue_frame(frame.payload - DH_FRAME_HEADER_SIZE,
                              DH_FRAME_HEADER_SIZE + frame.hdr.len);
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
    (void)state;

    const uint32_t now = channel_now_ms();

    /*
     * Whichever the session owes its helper: the beat that fills an idle
     * direction, or the announcement that a silent helper has just been
     * evicted. Never both, and nothing at all on the ordinary tick.
     */
    uint8_t owed[DH_FRAME_HEADER_SIZE + DH_SESSION_END_LEN];
    size_t owed_len = 0;
    if (dh_session_tick(&channel.session, now, owed, sizeof owed, &owed_len) == DH_FRAME_OK &&
        owed_len > 0)
        (void)channel_queue_frame(owed, owed_len);

    dh_pair_tick(&channel.pair, now);
    channel_pump_relay();
    channel_pump_out();
}
