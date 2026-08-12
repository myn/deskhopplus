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

typedef struct {
    uint32_t address;          // Address we're sending to the other box
    uint32_t checksum;
    uint16_t version;
    bool byte_done;            // Has the byte been successfully transferred
    bool upgrade_in_progress;  // True if firmware transfer from the other box is in progress
    bool image_dirty;          // A page has been written over the running image (#90)
    bool repair_attempted;     // A dirty image has already been given one restart (#90)
    uint32_t progressed_at_us; // When the upgrade last advanced (#90)
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
   False when no upgrade is running, so a caller may ask unconditionally. */
bool fw_upgrade_stalled(const fw_upgrade_state_t *fw, uint32_t now_us);
