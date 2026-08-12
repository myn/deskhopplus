/*
 * Whether a stored configuration is one this firmware may use, and how one is
 * made valid. Split out of load_config/save_config (#74).
 *
 * The split is the point. Both halves of this arithmetic used to live inside
 * functions that read memory-mapped flash and called write_flash_page, so
 * neither could run on the host — and when the range they checksummed became
 * wrong, it was wrong *identically* in both directions, so every round trip
 * through the pair still agreed with itself and nothing failed. Every setting
 * was lost on every boot for as long as that lasted.
 *
 * Pure C11: no flash, no SDK, no I/O. tests/config_test.c is the gate.
 */
#pragma once

#include <stdbool.h>

#include "config_layout.h"

/*
 * Stamp the checksum so this configuration will validate. What save_config
 * does immediately before writing the page.
 *
 * Idempotent: sealing an unchanged configuration twice leaves the same
 * checksum, because the checksum field is not part of what is summed.
 */
void config_seal(config_t *cfg);

/*
 * May this configuration be used? Magic, version and checksum, all three, and
 * all three because they fail differently: an erased or never-written sector
 * fails the magic, a config from another firmware fails the version, and a
 * disturbed one fails the checksum. A caller that gets false falls back to
 * defaults; it has nothing else it could usefully do, which is why this
 * answers a single question rather than reporting which check failed.
 */
bool config_is_valid(const config_t *cfg);
