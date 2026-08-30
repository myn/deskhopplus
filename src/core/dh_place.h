#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* C++ links these symbols too — the Windows helper is C++ (#53). */
#ifdef __cplusplus
extern "C" {
#endif

#define DH_PLACE_BODY_SIZE 5u
#define DH_POSITION_BODY_SIZE 5u

typedef struct {
    uint8_t chain_index;
    uint8_t chain_direction;
    uint8_t border_direction;
    uint16_t entry_position;
} dh_place;

typedef struct {
    uint8_t chain_index;
    uint16_t x;
    uint16_t y;
} dh_position;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} dh_display_rect;

typedef struct {
    int32_t x;
    int32_t y;
    size_t display_index;
} dh_place_point;

bool dh_place_encode(const dh_place *, uint8_t *, size_t);
bool dh_place_decode(const uint8_t *, size_t, dh_place *);
bool dh_position_encode(const dh_position *, uint8_t *, size_t);
bool dh_position_decode(const uint8_t *, size_t, dh_position *);
bool dh_place_target(const dh_place *, const dh_display_rect *, size_t, size_t,
                     dh_place_point *);

#ifdef __cplusplus
} /* extern "C" */
#endif
