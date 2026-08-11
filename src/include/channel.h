/*
 * The device's end of the helper channel (#45).
 *
 * All the decisions live in the shared core (src/core/dh_session.c, gated by
 * host tests); this is the plumbing that gives them a USB endpoint: reports in,
 * frames out, replies back, and a clock for the liveness timeout.
 *
 * Relaying bulk frames across the inter-board link is #47's, and pairing is
 * #46's. Both arrive on the same path.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "structs.h"

/* Reset the session — called when the channel's USB interface goes away. */
void channel_init(void);

/* One HID OUT report from the channel interface, verbatim. */
void channel_receive_report(const uint8_t *buffer, uint16_t bufsize);

/* channel_task — runs the liveness timeout and drains anything owed to the
   helper — is declared with the rest of the scheduler's tasks, in tasks.h. */

/*
 * A chord press was recorded before the last reboot: rotate the secret, open
 * the pairing window, and persist. Only meaningful in normal mode, which is
 * the only mode with a channel for a helper to be provisioned over.
 */
void channel_open_pairing_window(device_t *);
bool channel_pairing_window_owed(void);

/* One inter-board packet of a frame being relayed from the peer board. */
void handle_channel_relay_msg(uart_packet_t *, device_t *);

/* Is a helper live on this board's channel? The configuration UI shows it
   per side (#50) — the surface that survives the helper being disabled. */
bool channel_helper_present(void);
