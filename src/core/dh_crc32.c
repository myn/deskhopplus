/*
 * CRC32 (IEEE, reflected, as in zlib) — the per-chunk end-to-end integrity
 * check. Pure C11, no I/O, no platform dependencies.
 */

#include "dh_crc32.h"

/* Nibble-table variant: 64 bytes of table instead of 1 KiB, since this is
   also linked into the firmware image, which never computes payload CRCs. */
static const uint32_t crc32_nibble[16] = {
    0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4,
    0x4db26158, 0x5005713c, 0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
    0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
};

uint32_t dh_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        crc = (crc >> 4) ^ crc32_nibble[crc & 0x0f];
        crc = (crc >> 4) ^ crc32_nibble[crc & 0x0f];
    }
    return crc ^ 0xffffffffu;
}
