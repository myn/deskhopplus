/*
 * The board's end of the helper channel (#45), its per-frame authentication
 * (#111) and its relay across the inter-board link (#47). See include/channel.h.
 */

#include "main.h"

#include <pico/critical_section.h>
#include <pico/rand.h>

#include "channel_lifecycle.h"
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

static struct {
    channel_lifecycle lifecycle;
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

} channel;

void channel_lifecycle_lock(void) {
    critical_section_enter_blocking(&channel.out_lock);
}

void channel_lifecycle_unlock(void) {
    critical_section_exit(&channel.out_lock);
}

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

    if (load_identity(private_key) && dh_pair_set_identity(&channel.lifecycle.pair, private_key))
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
        if (!dh_pair_set_identity(&channel.lifecycle.pair, private_key))
            continue;
        save_identity(state, private_key);
        return;
    }
}

/* The registration this board holds, as stored. */
static void channel_load_registration(const device_t *state) {
    if (state->config.channel_paired)
        dh_pair_set_registration(&channel.lifecycle.pair, state->config.channel_helper_key_id,
                                 state->config.channel_shared_secret);
    else
        dh_pair_clear_registration(&channel.lifecycle.pair);
}

void channel_init(device_t *state) {
    if (!channel.locked) {
        critical_section_init(&channel.out_lock);
        channel.locked = true;
    }

    dh_session_init(&channel.lifecycle.session, CHANNEL_BUILD_TYPE);
    dh_pair_init(&channel.lifecycle.pair);

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
    dh_outq_init(&channel.lifecycle.out);
    critical_section_exit(&channel.out_lock);
    dh_relay_tx_init(&channel.lifecycle.relay_tx);
    dh_relay_rx_init(&channel.lifecycle.relay_rx, channel.lifecycle.relay_rx_buf, sizeof channel.lifecycle.relay_rx_buf);
    dh_inq_init(&channel.lifecycle.inbound);

    channel_load_identity(state);
    channel_load_registration(state);
    channel_lifecycle_link_lost(&channel.lifecycle);
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
    channel_lifecycle_link_lost(&channel.lifecycle);
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
    dh_pair_open_window(&channel.lifecycle.pair, channel_now_ms());

    /*
     * Clearing the session here is defensive, not an eviction anyone hears
     * about: the only caller is setup.c, on the normal-mode boot after the
     * chord's reboot, where channel_init has just cleared it anyway and no
     * helper has said hello. The helper re-pairs silently inside the window
     * it is standing in, having learned of the reboot from the USB
     * re-enumeration — a louder signal than any frame could be.
     */
    dh_session_drop(&channel.lifecycle.session);
}

/* Consumed once, on the normal-mode boot after a config chord. */
bool channel_pairing_window_owed(void) {
    if (watchdog_hw->scratch[3] != MAGIC_WORD_PAIR)
        return false;

    watchdog_hw->scratch[3] = 0;
    return true;
}

bool channel_helper_present(void) {
    return channel.lifecycle.session.present;
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
    const bool accepted = channel.lifecycle.cursor_query_origin == CURSOR_QUERY_NONE;
    if (accepted) {
        channel.lifecycle.cursor_query_origin = CURSOR_QUERY_LOCAL;
        channel.lifecycle.cursor_query_id = query_id;
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
    const bool accepted = channel.lifecycle.cursor_query_origin == CURSOR_QUERY_NONE;
    if (accepted) {
        channel.lifecycle.cursor_query_origin = CURSOR_QUERY_PEER;
        channel.lifecycle.cursor_query_id = packet->data[1];
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

static bool channel_emit_placement_body(uint8_t type, const uint8_t *body, size_t body_len) {
    return channel_lifecycle_emit_placement(&channel.lifecycle, type, body, body_len,
                                             channel_now_ms());
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
    if (dh_session_emit_relayed(&channel.lifecycle.session, &place, place_frame, sizeof place_frame,
                                &place_len) == DH_FRAME_OK &&
        dh_session_emit_relayed(&channel.lifecycle.session, &query, query_frame, sizeof query_frame,
                                &query_len) == DH_FRAME_OK &&
        dh_outq_offer_pair(&channel.lifecycle.out, place_frame, place_len,
                           query_frame, query_len) == DH_OUTQ_OK) {
        dh_session_note_sent(&channel.lifecycle.session, channel_now_ms());
        queued = true;
    }
    critical_section_exit(&channel.out_lock);
    return dh_txq_track(&channel.lifecycle.tx, queued);
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
    channel_lifecycle_receive_report(&channel.lifecycle, buffer, bufsize);
}

/* An authenticated position response, applied to this board's cursor state. */
void channel_lifecycle_position(void *context, const uint8_t *body, size_t body_len) {
    device_t *state = context;
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
    if (dh_relay_rx_push(&channel.lifecycle.relay_rx, &relayed, &frame) != DH_RELAY_OK)
        return; /* incomplete, or a loss the reassembler has already counted */

    /* Refused when core 0 is more than a pump batch behind, or when the frame
       is longer than any a transfer completes with. Counted either way, and
       the receiving helper re-requests a chunk — but not an offer, a done or a
       credit, which is why the ring is sized to make this rare (dh_inq.h). */
    const size_t total = DH_FRAME_HEADER_SIZE + frame.hdr.len;
    if (!dh_inq_stage(&channel.lifecycle.inbound, frame.payload - DH_FRAME_HEADER_SIZE, (uint16_t)total))
        return;

    /* The published slot is what core 0 reads, so the bytes must be visible
       before it moves. Cortex-M0+ retires in order, but the compiler is under
       no such obligation. */
    __dmb();
    dh_inq_publish(&channel.lifecycle.inbound);
}

void channel_lifecycle_barrier(void) {
    __dmb();
}

bool channel_lifecycle_send_relay(const dh_relay_packet *packet) {
    const enum packet_type_e type =
        packet->kind == DH_RELAY_PKT_START ? CHANNEL_START_MSG : CHANNEL_DATA_MSG;
    return queue_packet(packet->data, type, DH_RELAY_PAYLOAD);
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
    if (dh_outq_peek(&channel.lifecycle.out, &owed)) {
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
    dh_outq_advance(&channel.lifecycle.out, &owed, take);
    critical_section_exit(&channel.out_lock);
}

void channel_lifecycle_update_config(void *context, uint32_t now) {
    device_t *state = context;
    (void)now;
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
    state->_channel_reports_dropped = channel.lifecycle.reports_dropped;
    state->_channel_inbound_dropped = channel.lifecycle.inbound.dropped;
    state->_channel_outq_refused = channel.lifecycle.out.refused;
    state->_channel_relay_dropped = channel.lifecycle.tx.dropped;
    state->_channel_relay_orphans = channel.lifecycle.relay_rx.orphans;
    state->_channel_relay_truncated = channel.lifecycle.relay_rx.truncated;
    state->_channel_relay_refused = channel.lifecycle.relay_tx.q.refused;

    const dh_device_drops drops = {
        .reports = channel.lifecycle.reports_dropped,
        .inbound = channel.lifecycle.inbound.dropped,
        .outq = channel.lifecycle.out.refused,
        .unsent = channel.lifecycle.tx.dropped,
        .orphans = channel.lifecycle.relay_rx.orphans,
        .truncated = channel.lifecycle.relay_rx.truncated,
        .relay_q = channel.lifecycle.relay_tx.q.refused,
        .reports_in = channel.lifecycle.reports_in,
        .frames_in = channel.lifecycle.session.frames_in,
        .frames_refused = channel.lifecycle.session.frames_refused,
        .outq_priority = channel.lifecycle.out.refused_priority,
        .outq_bad_header = channel.lifecycle.out.refused_bad_header,
    };
    dh_session_set_drops(&channel.lifecycle.session, &drops);

    /*
     * The clipboard's two direction toggles, as the two verbs this board's own
     * helper acts on (#52). Set every pass rather than watched for changes:
     * the session sends a frame only when the value it holds is no longer this
     * one, so a config page write takes effect on a live session without
     * anything here having to notice that a setting moved.
     */
    dh_session_set_clip_policy(&channel.lifecycle.session,
                               dh_clip_policy_for(state->board_role,
                                                  state->config.clip_block_a_to_b != 0,
                                                  state->config.clip_block_b_to_a != 0),
                               state->config.clip_cap_mb);

}

void channel_task(device_t *state) {
    const uint32_t now = channel_now_ms();
    if (channel.config_wiped) {
        channel.config_wiped = false;
        channel_lifecycle_config_wiped(&channel.lifecycle, now);
    }
    if (dh_session_needs_nonce(&channel.lifecycle.session)) {
        uint8_t nonce[DH_NONCE_SIZE];
        channel_random_bytes(nonce, sizeof nonce);
        dh_session_stage_nonce(&channel.lifecycle.session, nonce);
    }
    channel_lifecycle_step(&channel.lifecycle, now, state);
    channel_pump_out();
}

void channel_lifecycle_save_registration(void *context) {
    device_t *state = context;
    memcpy(state->config.channel_helper_key_id, channel.lifecycle.pair.helper_key_id,
           sizeof state->config.channel_helper_key_id);
    memcpy(state->config.channel_shared_secret, channel.lifecycle.pair.shared_secret,
           sizeof state->config.channel_shared_secret);
    state->config.channel_paired = 1;
    save_config(state);
}

bool channel_lifecycle_query_unavailable(void *context, uint8_t query_id) {
    device_t *state = context;
    uart_packet_t unavailable = {
        .type = CURSOR_QUERY_UNAVAILABLE_MSG,
        .data = {(uint8_t)BOARD_ROLE, query_id},
    };
    return queue_uart_packet(&unavailable, state);
}
