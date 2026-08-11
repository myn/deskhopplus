/*
 * The device's end of the helper channel (#45). See include/channel.h.
 */

#include "main.h"

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

    /* One report owed to the helper. Every session reply fits in a single
       report — the largest is an 11-byte hello_ack — so this slice needs no
       queue. The fragmenting writer bulk needs arrives with the relay (#47). */
    uint8_t pending[CHANNEL_REPORT_SIZE];
    bool has_pending;
} channel;

void channel_init(void) {
    dh_session_init(&channel.session, CHANNEL_BUILD_TYPE);
    dh_frame_reader_init(&channel.reader);
    channel.has_pending = false;
}

bool channel_helper_present(void) {
    return channel.session.present;
}

static uint32_t channel_now_ms(void) {
    /* Wraps every ~49 days; the session's comparisons are wrap-safe. */
    return time_us_32() / 1000u;
}

/* Pad the tail: a report is a fixed 64 bytes with no length of its own, and
   DH_FRAME_PAD is what a decoder skips between frames (docs/protocol.md). */
static void channel_send(const uint8_t *frame, size_t len) {
    if (len == 0 || len > CHANNEL_REPORT_SIZE)
        return;

    if (channel.has_pending) {
        (void)dh_txq_track(&channel.tx, false);
        return;
    }

    memset(channel.pending, DH_FRAME_PAD, sizeof channel.pending);
    memcpy(channel.pending, frame, len);
    channel.has_pending = true;
    (void)dh_txq_track(&channel.tx, true);
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
            uint8_t reply[CHANNEL_REPORT_SIZE];
            size_t reply_len = 0;
            if (dh_session_on_frame(&channel.session, &frame, now, reply, sizeof reply,
                                    &reply_len) == DH_FRAME_OK)
                channel_send(reply, reply_len);
        } else if (consumed == 0) {
            break; /* nothing more to take from this report */
        }
    }
}

void channel_task(device_t *state) {
    (void)state;

    (void)dh_session_tick(&channel.session, channel_now_ms());

    if (!channel.has_pending || global_state.config_mode_active)
        return;

    /* The channel occupies the vendor interface slot in normal mode, with no
       report ID: a report is exactly one packet the framing layer owns. */
    if (!tud_hid_n_ready(ITF_NUM_HID_VENDOR))
        return;

    if (tud_hid_n_report(ITF_NUM_HID_VENDOR, 0, channel.pending, CHANNEL_REPORT_SIZE))
        channel.has_pending = false;
}
