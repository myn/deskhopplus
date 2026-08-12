/*
 * deskhopplus — configuration validation. See include/config_store.h.
 */

#include "config_store.h"

#include "dh_crc32.h"

/*
 * Everything ahead of the checksum, derived from offsetof rather than from
 * sizeof (#74). `sizeof(config_t) - sizeof(uint32_t)` looks equivalent and is
 * not: it excludes the checksum only while the checksum happens to be the last
 * four bytes of the struct, and adding a field can round config_t up and leave
 * padding *behind* it. offsetof is correct whatever the alignment leaves over,
 * and the static assertion in config_layout.h refuses the layout where the two
 * would disagree.
 *
 * dh_crc32 rather than the firmware's own calc_crc32: the two are the same
 * IEEE CRC32 and produce identical output (verified over random buffers before
 * the swap), and this one is already free of the firmware's flash headers,
 * which is what lets this file be compiled for the host at all.
 */
static uint32_t config_checksum(const config_t *cfg) {
    return dh_crc32((const uint8_t *)cfg, offsetof(config_t, checksum));
}

void config_seal(config_t *cfg) {
    cfg->checksum = config_checksum(cfg);
}

bool config_is_valid(const config_t *cfg) {
    return cfg->magic_header == CONFIG_MAGIC_HEADER && cfg->version == CURRENT_CONFIG_VERSION &&
           cfg->checksum == config_checksum(cfg);
}
