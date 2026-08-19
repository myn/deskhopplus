/*
 * deskhopplus — the board's stored identity. See include/identity_store.h.
 */

#include "identity_store.h"

#include "dh_crc32.h"

/*
 * Everything ahead of the checksum, derived from offsetof rather than from
 * sizeof, for the reason config_store.c gives: the two look equivalent and are
 * not, and the static assertion in identity_store.h refuses the layout where
 * they would disagree.
 */
static uint32_t identity_checksum(const identity_t *id) {
    return dh_crc32((const uint8_t *)id, offsetof(identity_t, checksum));
}

void identity_seal(identity_t *id) {
    id->checksum = identity_checksum(id);
}

bool identity_is_valid(const identity_t *id) {
    return id->magic_header == IDENTITY_MAGIC_HEADER && id->version == CURRENT_IDENTITY_VERSION &&
           id->checksum == identity_checksum(id);
}
