#pragma once

#include <stdbool.h>
#include <stdint.h>

#define DH_SEAM_RANGE_CAPACITY 4u
#define DH_SEAM_POSITION_MAX UINT16_MAX
#define DH_SEAM_CONFIG_FIELD_A_BASE 140u
#define DH_SEAM_CONFIG_FIELD_B_BASE 152u

typedef struct {
    uint8_t screen_index;
    uint8_t _reserved;
    uint16_t start;
    uint16_t end;
} dh_seam_range_t;

typedef struct {
    uint8_t segment;
    uint16_t position;
} dh_seam_hit_t;

typedef enum {
    DH_SEAM_CROSSING_LEGACY,
    DH_SEAM_CROSSING_MAPPED,
    DH_SEAM_CROSSING_BLOCKED,
} dh_seam_crossing_kind_t;

typedef struct {
    uint8_t screen_index;
    uint16_t position;
} dh_seam_entry_t;

int32_t dh_seam_map_coordinate(int32_t position, int32_t source_start, int32_t source_end,
                               int32_t target_start, int32_t target_end);

bool dh_seam_map_is_configured(const dh_seam_range_t ranges[DH_SEAM_RANGE_CAPACITY]);
bool dh_seam_cross(const dh_seam_range_t ranges[DH_SEAM_RANGE_CAPACITY],
                   uint32_t screen_index, uint16_t position, dh_seam_hit_t *hit);
bool dh_seam_entry(const dh_seam_range_t ranges[DH_SEAM_RANGE_CAPACITY],
                   uint8_t segment, uint16_t position, uint16_t *entry);
dh_seam_crossing_kind_t dh_seam_resolve_crossing(
    const dh_seam_range_t source[DH_SEAM_RANGE_CAPACITY],
    const dh_seam_range_t target[DH_SEAM_RANGE_CAPACITY],
    uint32_t source_screen_index, uint32_t source_screen_count, uint32_t target_screen_count,
    uint16_t source_position, dh_seam_entry_t *entry);
