/*
 * The state of a firmware upgrade being received, and whether it is still
 * alive (#90).
 *
 * Two paths write this struct, and both can stop without saying so:
 *
 *   - the *pull*, where this board requests the image word by word from a
 *     newer peer board over the inter-board link (handlers.c, tasks.c);
 *   - the *UF2 drop*, where the host writes it as blocks onto the mass
 *     storage disk that only exists in config mode (ramdisk.c).
 *
 * Before this, `upgrade_in_progress` was set by both and cleared only by the
 * transfer finishing, or by a response arriving for the wrong address. A
 * transfer that simply stopped left the flag set for good, and everything
 * behind it in `heartbeat_output_task` — the heartbeat, the config-mode
 * timeout, the config-mode LED — stopped with it. The board went silent to
 * its peer, could never retry, could not time out of config mode, and looked
 * perfectly healthy while doing it. Recovery was a power cycle.
 *
 * So an upgrade now records when it last advanced, and a caller can ask
 * whether it has gone quiet for long enough to be abandoned.
 *
 * Pure C11: no SDK, no I/O, no clock of its own — the caller supplies the
 * time. tests/fw_upgrade_test.c is the gate. Same split, for the same reason,
 * as peer_fw.c (#89) and config_store.c (#74).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * How long an upgrade may make no progress before it is abandoned.
 *
 * Both paths advance far faster than this when they are alive: the pull asks
 * for the next four bytes at up to 4 kHz, and UF2 blocks arrive at USB mass
 * storage speed. Thirty seconds of nothing is not a slow link, it is a dead
 * one.
 *
 * Deliberately generous all the same, because abandoning costs something even
 * when it recovers: the pull restarts from address 0 and re-sends the whole
 * 256 KB image. The observed hang ran for over fourteen minutes, so a window
 * this wide still fixes it outright.
 */
#define FW_UPGRADE_STALL_US (30u * 1000000u)

/*
 * How long to wait for a requested word before asking again.
 *
 * The pull has no acknowledgement and no retransmission beneath it, and a
 * request or its response can be dropped at either end: `uart_tx_queue` is
 * shared with mouse and keyboard traffic, and both `request_byte` and
 * `handle_request_byte_msg` enqueue into it. A single drop used to mean the
 * receiving board waited forever for a word that was never coming, because
 * `byte_done` is cleared by the request and set only by the response.
 *
 * Measured on hardware 2026-08-12: moving the cursor across the seam during a
 * pull was enough to lose responses repeatedly, and each loss cost a full
 * 30 s stall and a restart from address 0. Two of those in one transfer is
 * ROM recovery, which is how a board ended up in BOOTSEL for what was really
 * a dropped packet.
 *
 * A UART round trip is well under a millisecond, so 100 ms is around a
 * hundredfold headroom, and it leaves room for three hundred attempts inside
 * the stall window — which is what turns that window back into a backstop for
 * a genuinely dead link rather than the routine cost of a busy one.
 */
#define FW_UPGRADE_REREQUEST_US (100u * 1000u)

/*
 * Which transport is delivering the upgrade being received.
 *
 * Two exist, and they share nothing but this struct: the *pull* asks the peer
 * board for the image a word at a time over the inter-board link, and the
 * *UF2 drop* takes it from the host as blocks on the config-mode disk.
 *
 * They used to be told apart by nothing at all. `upgrade_in_progress` said
 * only *that* a transfer was running, and firmware_upgrade_task — the pull —
 * took that flag as its cue to start asking. A drop was safe only by accident:
 * the pull also stood down while `byte_done` was clear, and a drop never sets
 * it. Re-requesting replaced that second guard with its opposite, since a word
 * that has not come back is now the cue to ask again. So a drop began making
 * this board pull its peer's entire image on top of the one the host was
 * writing: two writers on one flash, two transports summing into one checksum,
 * and the pull's own end-of-transfer checksum failing and handing the board to
 * ROM (#104).
 *
 * The transport is therefore recorded rather than inferred. NONE is zero,
 * which is what a zeroed struct and a board with nothing in flight both mean.
 */
typedef enum {
    FW_UPGRADE_SOURCE_NONE = 0, // Nothing is being received
    FW_UPGRADE_SOURCE_PULL,     // Word by word from the peer board over UART
    FW_UPGRADE_SOURCE_DROP,     // UF2 blocks from the host over mass storage
} fw_upgrade_source_t;

typedef struct {
    uint32_t address;           // Address we're sending to the other box
    uint32_t checksum;
    uint16_t version;
    bool byte_done;             // Has the byte been successfully transferred
    bool upgrade_in_progress;   // True if a transfer is being received, by either transport
    fw_upgrade_source_t source; // Which transport is delivering it (#104)
    bool image_dirty;           // A page has been written over the running image (#90)
    uint32_t progressed_at_us;  // When the upgrade last advanced (#90)
    uint32_t requested_at_us;   // When the outstanding word was last asked for (#90)
} fw_upgrade_state_t;

/*
 * The upgrade advanced: it just started, a requested word came back, or a UF2
 * block was written. Called on every step rather than every page, since it is
 * the *absence* of steps that this exists to notice.
 *
 * Deliberately 32-bit, where peer_fw.c's equivalent is 64-bit, because this
 * one is written on core0 by the UF2 drop and read on core1 by the heartbeat
 * task. ARMv6-M has no 64-bit atomic load, so a torn read across the 32-bit
 * boundary would hand back a timestamp over an hour in the past and abandon a
 * live transfer. A single word cannot tear, and the wrap is handled below —
 * the same trick, for the same reason, as `core1_last_loop_pass`.
 */
void fw_upgrade_progress(fw_upgrade_state_t *fw, uint32_t now_us);

/* Whether an in-flight upgrade has gone quiet long enough to give up on.
   False when no upgrade is running, so a caller may ask unconditionally.

   Re-requesting is deliberately not progress: a board that asks into the void
   forever must still reach this, or the backstop would never fire. Only a
   response that arrives calls fw_upgrade_progress. */
bool fw_upgrade_stalled(const fw_upgrade_state_t *fw, uint32_t now_us);

/*
 * Whether the pull may act: a transfer is running, and it is the pull's own.
 *
 * The one question to ask before sending a request, before accepting a
 * response, and before writing a page on the pull's behalf. A drop answers no
 * to all three, which is the whole of #104: it can then neither be advanced by
 * a response that was never asked for, nor made to ask.
 *
 * False when nothing is running at all, so a caller may ask unconditionally.
 */
bool fw_upgrade_may_pull(const fw_upgrade_state_t *fw);

/* Whether the outstanding word has gone unanswered long enough to ask again.
   False when nothing is outstanding *and* when the transfer is not a pull, so
   a caller may ask unconditionally: only a pull has words outstanding. */
bool fw_upgrade_request_lost(const fw_upgrade_state_t *fw, uint32_t now_us);

/*
 * Whether giving up on this upgrade has to hand the board to ROM, rather than
 * simply let the pull start over.
 *
 * Restarting *is* the repair — a pull runs from address 0 and writes every
 * page, so a run that completes overwrites whatever a previous one left
 * half-written. While the peer board is still there, another attempt is
 * always the better move: it costs nothing but time, and the alternative
 * costs the user a manual reflash. An earlier version of this spent one
 * restart and then recovered to ROM, which on real hardware meant a board in
 * BOOTSEL twice over for what were dropped packets on a busy link.
 *
 * A peer board that has gone is the case where no amount of retrying helps.
 * Nothing can repair the image, and leaving a half-written one to be booted
 * later is the failure this issue exists to prevent — so that, and only that,
 * is loud.
 *
 * `peer_present` is peer_fw.h's question: a heartbeat inside PEER_FW_STALE_US.
 */
bool fw_upgrade_must_recover(const fw_upgrade_state_t *fw, bool peer_present);
