#pragma once

#include <stdint.h>

#include "dh_config_text.h"

typedef struct {
    dh_key_override_t overrides[DH_CONFIG_TEXT_OVERRIDE_CAPACITY];
    uint8_t override_count;
    uint8_t passthrough[DH_CONFIG_TEXT_PASSTHROUGH_CAPACITY];
    uint8_t passthrough_count;
} dh_keymap_profile_t;

/* One definition for the config API and generated web page. The API carries
   at most seven data bytes per field, so each profile occupies twelve fields. */
#define DH_KEYMAP_OVERRIDE_COUNT_OFFSET    64u
#define DH_KEYMAP_PASSTHROUGH_OFFSET       65u
#define DH_KEYMAP_PASSTHROUGH_COUNT_OFFSET 81u
#define DH_KEYMAP_PROFILE_SIZE             82u
#define DH_KEYMAP_CONFIG_CHUNK_SIZE         7u
#define DH_KEYMAP_CONFIG_CHUNK_COUNT       12u
#define DH_KEYMAP_CONFIG_FIELD_OFFSET      14u
#define DH_KEYMAP_CONFIG_FIELD_A_BASE      24u
#define DH_KEYMAP_CONFIG_FIELD_B_BASE      54u

_Static_assert(sizeof(dh_keymap_profile_t) == DH_KEYMAP_PROFILE_SIZE,
               "dh_keymap_profile_t wire layout changed");
_Static_assert((DH_KEYMAP_CONFIG_CHUNK_COUNT - 1u) * DH_KEYMAP_CONFIG_CHUNK_SIZE <
                   DH_KEYMAP_PROFILE_SIZE &&
                   DH_KEYMAP_CONFIG_CHUNK_COUNT * DH_KEYMAP_CONFIG_CHUNK_SIZE >=
                       DH_KEYMAP_PROFILE_SIZE,
               "keymap config chunks must cover the profile exactly once");

#define DH_KEYMAP_CONFIG_CHUNKS(X, base, screen_idx)                                      \
    X(base + 0, screen_idx, 0 * DH_KEYMAP_CONFIG_CHUNK_SIZE, DH_KEYMAP_CONFIG_CHUNK_SIZE), \
    X(base + 1, screen_idx, 1 * DH_KEYMAP_CONFIG_CHUNK_SIZE, DH_KEYMAP_CONFIG_CHUNK_SIZE), \
    X(base + 2, screen_idx, 2 * DH_KEYMAP_CONFIG_CHUNK_SIZE, DH_KEYMAP_CONFIG_CHUNK_SIZE), \
    X(base + 3, screen_idx, 3 * DH_KEYMAP_CONFIG_CHUNK_SIZE, DH_KEYMAP_CONFIG_CHUNK_SIZE), \
    X(base + 4, screen_idx, 4 * DH_KEYMAP_CONFIG_CHUNK_SIZE, DH_KEYMAP_CONFIG_CHUNK_SIZE), \
    X(base + 5, screen_idx, 5 * DH_KEYMAP_CONFIG_CHUNK_SIZE, DH_KEYMAP_CONFIG_CHUNK_SIZE), \
    X(base + 6, screen_idx, 6 * DH_KEYMAP_CONFIG_CHUNK_SIZE, DH_KEYMAP_CONFIG_CHUNK_SIZE), \
    X(base + 7, screen_idx, 7 * DH_KEYMAP_CONFIG_CHUNK_SIZE, DH_KEYMAP_CONFIG_CHUNK_SIZE), \
    X(base + 8, screen_idx, 8 * DH_KEYMAP_CONFIG_CHUNK_SIZE, DH_KEYMAP_CONFIG_CHUNK_SIZE), \
    X(base + 9, screen_idx, 9 * DH_KEYMAP_CONFIG_CHUNK_SIZE, DH_KEYMAP_CONFIG_CHUNK_SIZE), \
    X(base + 10, screen_idx, 10 * DH_KEYMAP_CONFIG_CHUNK_SIZE, DH_KEYMAP_CONFIG_CHUNK_SIZE), \
    X(base + 11, screen_idx, 11 * DH_KEYMAP_CONFIG_CHUNK_SIZE,                            \
      DH_KEYMAP_PROFILE_SIZE - 11 * DH_KEYMAP_CONFIG_CHUNK_SIZE)
