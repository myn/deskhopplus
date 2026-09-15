#include "dh_bundle.h"

#include <string.h>

static bool kind_is_written(uint8_t kind) {
    return kind == DH_BUNDLE_PART_TEXT || kind == DH_BUNDLE_PART_PNG;
}

size_t dh_bundle_packed_len(const dh_bundle_part *parts, uint8_t count) {
    if (parts == NULL) return 0;
    size_t total = 0;
    for (uint8_t i = 0; i < count; i++) total += DH_BUNDLE_PART_HEAD + parts[i].len;
    return total;
}

size_t dh_bundle_pack(const dh_bundle_part *parts, uint8_t count, uint8_t *out, size_t cap) {
    if (parts == NULL || out == NULL || count == 0 || count > DH_BUNDLE_PARTS_MAX) return 0;
    if (dh_bundle_packed_len(parts, count) > cap) return 0;

    size_t at = 0;
    for (uint8_t i = 0; i < count; i++) {
        const dh_bundle_part *part = &parts[i];
        out[at++] = part->kind;
        out[at++] = (uint8_t)(part->len);
        out[at++] = (uint8_t)(part->len >> 8);
        out[at++] = (uint8_t)(part->len >> 16);
        out[at++] = (uint8_t)(part->len >> 24);
        if (part->len > 0) memcpy(out + at, part->bytes, part->len);
        at += part->len;
    }
    return at;
}

bool dh_bundle_unpack(const uint8_t *payload, size_t len, dh_bundle *out) {
    if (payload == NULL || out == NULL) return false;
    out->count = 0;

    size_t at = 0;
    while (at < len) {
        if (len - at < DH_BUNDLE_PART_HEAD) return false;
        const uint8_t kind = payload[at];
        const uint32_t part_len = (uint32_t)payload[at + 1] | ((uint32_t)payload[at + 2] << 8) |
                                  ((uint32_t)payload[at + 3] << 16) |
                                  ((uint32_t)payload[at + 4] << 24);
        at += DH_BUNDLE_PART_HEAD;
        /* The one refusal that matters: a length that promises bytes the
           payload does not hold would be read past its end. */
        if (part_len > len - at) return false;

        if (kind_is_written(kind)) {
            if (out->count >= DH_BUNDLE_PARTS_MAX) return false;
            out->parts[out->count].kind = kind;
            out->parts[out->count].bytes = payload + at;
            out->parts[out->count].len = part_len;
            out->count++;
        }
        at += part_len;
    }
    return out->count > 0;
}
