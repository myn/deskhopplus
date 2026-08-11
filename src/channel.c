/*
 * The device's end of the helper channel (#45) and its relay across the
 * inter-board link (#47). See include/channel.h.
 */

#include "main.h"

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
    dh_frame_reader reader;
    dh_txq_stats tx; /* replies lost to a busy endpoint, never silently */

    /*
     * Outbound to this board's helper: one frame at a time, emitted a report
     * per tick. Session replies fit in a single report; a frame relayed from
     * the peer does not, so both go through the same writer.
     */
    uint8_t out[DH_FRAME_MAX_SIZE];
    uint16_t out_len;
    uint16_t out_sent;

    /* The relay to and from the peer board, with the storage it owns. */
    dh_relay_tx relay_tx;
    dh_relay_rx relay_rx;
    uint8_t relay_priority[DH_RELAY_PRIORITY_MAX];
    uint8_t relay_bulk[DH_FRAME_MAX_SIZE];
    uint8_t relay_rx_buf[DH_FRAME_MAX_SIZE];
} channel;

void channel_init(void) {
    dh_session_init(&channel.session, CHANNEL_BUILD_TYPE);
    dh_frame_reader_init(&channel.reader);
    dh_relay_tx_init(&channel.relay_tx, channel.relay_priority, sizeof channel.relay_priority,
                     channel.relay_bulk, sizeof channel.relay_bulk);
    dh_relay_rx_init(&channel.relay_rx, channel.relay_rx_buf, sizeof channel.relay_rx_buf);
    channel.out_len = 0;
    channel.out_sent = 0;
}

bool channel_helper_present(void) {
    return channel.session.present;
}

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

/* Hand a whole frame to this board's helper. One at a time: the credit window
   upstream is what keeps a second from arriving before this one drains. */
static bool channel_queue_frame(const uint8_t *frame, size_t len) {
    if (len == 0 || len > sizeof channel.out)
        return dh_txq_track(&channel.tx, false);

    if (channel.out_len != 0)
        return dh_txq_track(&channel.tx, false);

    memcpy(channel.out, frame, len);
    channel.out_len = (uint16_t)len;
    channel.out_sent = 0;
    return dh_txq_track(&channel.tx, true);
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
               trustworthy and the helper reconnects (docs/protocol.md). */
            dh_session_drop(&channel.session);
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
            if (dh_msg_is_bulk(frame.hdr.type)) {
                (void)dh_relay_tx_offer(&channel.relay_tx,
                                        frame.payload - DH_FRAME_HEADER_SIZE,
                                        DH_FRAME_HEADER_SIZE + frame.hdr.len);
                continue;
            }

            uint8_t reply[DH_FRAME_MAX_SIZE];
            size_t reply_len = 0;
            if (dh_session_on_frame(&channel.session, &frame, now, reply, sizeof reply,
                                    &reply_len) == DH_FRAME_OK &&
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
    if (channel.out_len == 0 || global_state.config_mode_active)
        return;

    /* The channel occupies the vendor interface slot in normal mode, with no
       report ID: a report is exactly one packet the framing layer owns. */
    if (!tud_hid_n_ready(ITF_NUM_HID_VENDOR))
        return;

    uint8_t report[CHANNEL_REPORT_SIZE];
    const uint16_t remaining = (uint16_t)(channel.out_len - channel.out_sent);
    const uint16_t take = remaining < CHANNEL_REPORT_SIZE ? remaining : CHANNEL_REPORT_SIZE;

    /* Pad the tail: a report is a fixed 64 bytes with no length of its own,
       and DH_FRAME_PAD is what a decoder skips between frames. */
    memset(report, DH_FRAME_PAD, sizeof report);
    memcpy(report, channel.out + channel.out_sent, take);

    if (!tud_hid_n_report(ITF_NUM_HID_VENDOR, 0, report, CHANNEL_REPORT_SIZE))
        return;

    channel.out_sent = (uint16_t)(channel.out_sent + take);
    if (channel.out_sent >= channel.out_len) {
        channel.out_len = 0;
        channel.out_sent = 0;
    }
}

void channel_task(device_t *state) {
    (void)state;

    (void)dh_session_tick(&channel.session, channel_now_ms());
    channel_pump_relay();
    channel_pump_out();
}
