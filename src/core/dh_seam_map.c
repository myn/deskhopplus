#include "dh_seam_map.h"

int32_t dh_seam_map_coordinate(int32_t position, int32_t source_start, int32_t source_end,
                               int32_t target_start, int32_t target_end) {
    const int32_t source_size = source_end - source_start;
    const int32_t target_size = target_end - target_start;
    if (source_size <= 0)
        return position;
    if (target_size <= 0)
        return target_start;
    if (position <= source_start)
        return target_start;
    if (position >= source_end)
        return target_end;
    return target_start + (int32_t)((((int64_t)position - source_start) * target_size +
                                     source_size / 2) /
                                    source_size);
}

static bool range_is_configured(const dh_seam_range_t *range) {
    return range->screen_index != 0 && range->start < range->end;
}

static bool range_is_available(const dh_seam_range_t *range, uint32_t screen_count) {
    return range_is_configured(range) && range->screen_index <= screen_count;
}

dh_seam_crossing_kind_t dh_seam_resolve_crossing(
    const dh_seam_range_t source[DH_SEAM_RANGE_CAPACITY],
    const dh_seam_range_t target[DH_SEAM_RANGE_CAPACITY],
    uint32_t source_screen_index, uint32_t source_screen_count, uint32_t target_screen_count,
    uint16_t source_position, dh_seam_entry_t *entry) {
    dh_seam_hit_t hit;
    if (dh_seam_cross(source, source_screen_index, source_position, &hit)) {
        if (!range_is_available(&source[hit.segment], source_screen_count) ||
            !range_is_available(&target[hit.segment], target_screen_count) ||
            !dh_seam_entry(target, hit.segment, hit.position, &entry->position))
            return DH_SEAM_CROSSING_LEGACY;
        entry->screen_index = target[hit.segment].screen_index;
        return DH_SEAM_CROSSING_MAPPED;
    }

    for (uint8_t i = 0; i < DH_SEAM_RANGE_CAPACITY; i++)
        if (range_is_available(&source[i], source_screen_count) &&
            range_is_available(&target[i], target_screen_count))
            return DH_SEAM_CROSSING_BLOCKED;
    return DH_SEAM_CROSSING_LEGACY;
}

bool dh_seam_map_is_configured(const dh_seam_range_t ranges[DH_SEAM_RANGE_CAPACITY]) {
    for (uint8_t i = 0; i < DH_SEAM_RANGE_CAPACITY; i++)
        if (range_is_configured(&ranges[i]))
            return true;
    return false;
}

bool dh_seam_cross(const dh_seam_range_t ranges[DH_SEAM_RANGE_CAPACITY],
                   uint32_t screen_index, uint16_t position, dh_seam_hit_t *hit) {
    const dh_seam_range_t *match = 0;
    uint8_t segment = 0;
    for (uint8_t i = 0; i < DH_SEAM_RANGE_CAPACITY; i++) {
        const dh_seam_range_t *candidate = &ranges[i];
        if (range_is_configured(candidate) && candidate->screen_index == screen_index &&
            position >= candidate->start && position <= candidate->end) {
            match = candidate;
            segment = i;
        }
    }
    if (!match)
        return false;

    hit->segment = segment;
    hit->position = (uint16_t)dh_seam_map_coordinate(
        position, match->start, match->end, 0, DH_SEAM_POSITION_MAX);
    return true;
}

bool dh_seam_entry(const dh_seam_range_t ranges[DH_SEAM_RANGE_CAPACITY],
                   uint8_t segment, uint16_t position, uint16_t *entry) {
    if (segment >= DH_SEAM_RANGE_CAPACITY || !range_is_configured(&ranges[segment]))
        return false;

    const dh_seam_range_t *range = &ranges[segment];
    *entry = (uint16_t)dh_seam_map_coordinate(
        position, 0, DH_SEAM_POSITION_MAX, range->start, range->end);
    return true;
}
