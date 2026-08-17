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

/* One-time setup, at boot. */
void channel_init(void);

/* The USB interface went away: drop the connection but keep any open pairing
   window, which belongs to the user's chord press rather than to the link. */
void channel_link_lost(void);

/* The configuration was wiped, taking the secret with it: end the session and
   forget the pairing rather than go on authenticating against a secret that no
   longer exists in flash (#75). Callable from either core; the work itself
   lands on core 0's next tick. */
void channel_config_wiped(void);

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
