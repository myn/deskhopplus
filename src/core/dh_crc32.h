/*
 * deskhopplus shared core — CRC32 (IEEE, reflected, as in zlib).
 * The per-chunk end-to-end integrity check of the reliability model.
 * Pure C11, no I/O, no platform dependencies.
 */

#ifndef DH_CRC32_H_
#define DH_CRC32_H_

#include <stddef.h>
#include <stdint.h>

uint32_t dh_crc32(const uint8_t *data, size_t len);

#endif /* DH_CRC32_H_ */
