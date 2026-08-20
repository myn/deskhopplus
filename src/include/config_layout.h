/*
 * The stored configuration's layout, and the two constants that identify it.
 *
 * Split out of structs.h (#74) so that the validation decision in
 * config_store.h can be compiled and tested on the host. structs.h itself
 * cannot: device_t reaches into the Pico SDK, and the defect this split
 * exists to prevent was pure arithmetic sitting behind flash I/O, where no
 * test could reach it.
 *
 * Pure C11 — stdint, stddef, and headers that are themselves pure. Keep it
 * that way, or the host test goes with it.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "constants.h" /* NUM_SCREENS */
#include "dh_p256.h"   /* DH_P256_SHARED_SIZE, DH_KEY_ID_SIZE */
#include "screen.h"    /* output_t */

/* What marks these bytes as a configuration at all. An erased sector reads as
   0xFF everywhere and a fresh one as 0x00, and neither is this. */
#define CONFIG_MAGIC_HEADER 0xB00B1E5u

/*
 * Bumped whenever the field layout changes, so a config written by another
 * firmware is refused rather than read with the wrong shape.
 *
 * Bumping it is not free: every stored configuration stops validating and
 * every device falls back to defaults on the next boot. That costs the user
 * every setting *and* the pairing secret, so each bump is a chord press on
 * both boards before the helpers work again. Weigh that before changing it.
 *
 *   9: the channel pairing secret joined config_t (#46). An older
 *      configuration is not read as a newer one — it falls back to defaults,
 *      which is also how a device with no secret ends up needing one chord
 *      press.
 *  10: the bearer secret became a registration — the paired helper's key id
 *      and the shared secret one ECDH produced (#111, ADR-0008). The bump
 *      costs every board its pairing, and that is not a side effect: v2 does
 *      not migrate a v1 pairing, because a migration path would have to accept
 *      the bearer token, which is the thing being removed. One chord press.
 */
#define CURRENT_CONFIG_VERSION 10

typedef struct {
    uint32_t magic_header;
    uint32_t version;

    uint8_t force_mouse_boot_mode;
    uint8_t force_kbd_boot_protocol;

    uint8_t kbd_led_as_indicator;
    uint8_t hotkey_toggle;
    uint8_t enable_acceleration;

    uint8_t enforce_ports;
    uint16_t jump_threshold;

    output_t output[NUM_SCREENS];

    /*
     * The registration: the one helper key this board has paired with (#111).
     * `channel_helper_key_id` names that key without carrying it, and
     * `channel_shared_secret` is what the single pairing-time ECDH produced —
     * every session key is derived from it, and it never crosses the wire.
     *
     * `channel_shared_secret` is deliberately *not* in api_field_map: config is
     * only ever exposed field by field, so material kept out of that map is
     * unreadable through the API and never syncs to the peer board.
     *
     * `channel_helper_key_id` is in the map, read-only, as fields 86 and 87 —
     * it is not a secret (every hello carries it in clear) and the config page
     * showing it is the only answer anywhere to "what is paired to this board?"
     * (#114). Read-only is what keeps the property above: the page writes a
     * changed field to both boards, so a *writable* registration field would
     * let a re-pairing here evict the other computer's helper.
     *
     * Wiped with the rest of the configuration, because wiping is how a user
     * unpairs. The board's *identity* is not here: it lives in its own flash
     * sector (flash_layout.h) precisely so that a wipe unpairs without
     * changing who this board is.
     */
    uint8_t channel_helper_key_id[DH_KEY_ID_SIZE];
    uint8_t channel_shared_secret[DH_P256_SHARED_SIZE];
    uint8_t channel_paired;

    /* Named, not left to the compiler: _reserved needs 4-byte alignment, so
       three bytes sit here either way — and they are inside the checksummed
       range. As padding their contents are indeterminate, so a config_t built
       by aggregate initialisation rather than copied whole would be sealed
       over undefined bytes and refused on the next boot. That is #74's
       failure mode with a different cause. A named member is zeroed by
       initialisation and copied deterministically, which removes it. */
    uint8_t _pad[3];

    /* Two words, so the struct ends exactly on the checksum. #46's 17 bytes
       rounded config_t up to 160 and left four bytes of padding *behind* the
       checksum, which is what broke persistence (#74); absorbing them here
       keeps the tail intentional rather than whatever alignment happens to
       leave over. */
    uint32_t _reserved[2];

    // Keep checksum at the end of the struct
    uint32_t checksum;
} config_t;

/*
 * The checksum must be the last field *and* leave no trailing padding, because
 * both save_config and load_config checksum everything ahead of it (#74).
 *
 * This is asserted rather than trusted: config_t inherits 8-byte alignment from
 * the uint64_t timers in screensaver_t, so adding a field can round the struct
 * up and silently leave padding behind the checksum. #46 did exactly that —
 * sizeof went 136 -> 160 with the checksum at 152 — and the CRC then covered
 * the checksum field itself, so no stored config ever validated again and every
 * boot fell back to defaults.
 */
_Static_assert(offsetof(config_t, checksum) == sizeof(config_t) - sizeof(uint32_t),
               "config_t: checksum must be last, with no trailing padding (see #74)");
