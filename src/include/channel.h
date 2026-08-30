/*
 * The board's end of the helper channel (#45).
 *
 * All the decisions live in the shared core (src/core/dh_session.c, gated by
 * host tests); this is the plumbing that gives them a USB endpoint: reports in,
 * frames out, replies back, a clock for the liveness timeout, and the two
 * things a pure core cannot have — flash for the board's identity, and entropy
 * for its private key and its session nonces.
 *
 * Relaying bulk frames across the inter-board link is #47's, and pairing is
 * #111's. Both arrive on the same path, and under v2 both are authorised per
 * frame rather than per session (ADR-0008).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "structs.h"

/* One-time setup, at boot. Reads the board's identity out of its own flash
   sector and generates one on first boot, which costs ~134 ms — free here,
   before the scheduler starts, and nowhere else on this board. */
void channel_init(device_t *);

/* The USB interface went away: drop the connection but keep any open pairing
   window, which belongs to the user's chord press rather than to the link. */
void channel_link_lost(void);

/* The configuration was wiped, taking the registration with it: end the
   session and unpair rather than go on authenticating against a shared secret
   that no longer exists in flash (#75). The board's *identity* is untouched —
   it lives in its own sector, so a wipe unpairs without changing who this board
   is. Callable from either core; the work itself lands on core 0's next tick. */
void channel_config_wiped(void);

/* One HID OUT report from the channel interface, verbatim. Copied and nothing
   more: every decision behind it, and all of v2's cryptography, runs in
   channel_task instead — off the USB callback's already-deep stack. */
void channel_receive_report(const uint8_t *buffer, uint16_t bufsize);

/* channel_task — runs the liveness timeout and drains anything owed to the
   helper — is declared with the rest of the scheduler's tasks, in tasks.h. */

/*
 * A chord press was recorded before the last reboot: open the pairing window.
 * Only meaningful in normal mode, which is the only mode with a channel for a
 * helper to be provisioned over.
 *
 * Nothing is rotated and nothing is persisted, unlike v1. Only public halves
 * cross now, so there is no secret a window has to replace — and a chord press
 * nobody pairs against leaves the existing registration exactly as it was.
 */
void channel_open_pairing_window(void);
bool channel_pairing_window_owed(void);

/* One inter-board packet of a frame being relayed from the peer board. */
void handle_channel_relay_msg(uart_packet_t *, device_t *);

/* Is a helper live on this board's channel? The configuration UI shows it
   per side (#50) — the surface that survives the helper being disabled. */
bool channel_helper_present(void);

/* Ask the helper on `output` to place the entry cursor on a mapped monitor.
   Fire-and-forget: HID positioning remains the fallback when no helper is live. */
void channel_place_cursor(uint8_t output, uint8_t screen, uint8_t chain, uint8_t border,
                          uint16_t position);
