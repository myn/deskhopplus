#include "dh_place.h"
#include "dh_mouse_layout.h"

static uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static void wr_u16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

bool dh_place_encode(const dh_place *place, uint8_t *out, size_t cap) {
    if (place == NULL || out == NULL || cap < DH_PLACE_BODY_SIZE)
        return false;
    out[0] = place->chain_index;
    out[1] = place->chain_direction;
    out[2] = place->border_direction;
    wr_u16(out + 3, place->entry_position);
    return true;
}

bool dh_place_decode(const uint8_t *body, size_t len, dh_place *place) {
    if (body == NULL || place == NULL || len != DH_PLACE_BODY_SIZE)
        return false;
    place->chain_index = body[0];
    place->chain_direction = body[1];
    place->border_direction = body[2];
    place->entry_position = rd_u16(body + 3);
    return true;
}

bool dh_position_encode(const dh_position *position, uint8_t *out, size_t cap) {
    if (position == NULL || out == NULL || cap < DH_POSITION_BODY_SIZE)
        return false;
    out[0] = position->query_id;
    out[1] = position->chain_index;
    wr_u16(out + 2, position->x);
    wr_u16(out + 4, position->y);
    return true;
}

bool dh_position_decode(const uint8_t *body, size_t len, dh_position *position) {
    if (body == NULL || position == NULL || len != DH_POSITION_BODY_SIZE)
        return false;
    position->query_id = body[0];
    position->chain_index = body[1];
    position->x = rd_u16(body + 2);
    position->y = rd_u16(body + 4);
    return true;
}

static int64_t center_x(const dh_display_rect *r) { return (int64_t)r->x * 2 + r->width; }
static int64_t center_y(const dh_display_rect *r) { return (int64_t)r->y * 2 + r->height; }

static bool lies_in_direction(const dh_display_rect *from, const dh_display_rect *candidate,
                              uint8_t direction) {
    if (direction == DH_DIRECTION_LEFT) return center_x(candidate) < center_x(from);
    if (direction == DH_DIRECTION_RIGHT) return center_x(candidate) > center_x(from);
    if (direction == DH_DIRECTION_TOP) return center_y(candidate) < center_y(from);
    if (direction == DH_DIRECTION_BOTTOM) return center_y(candidate) > center_y(from);
    return false;
}

static uint64_t directional_distance(const dh_display_rect *from,
                                     const dh_display_rect *candidate, uint8_t direction) {
    const int64_t delta = (direction == DH_DIRECTION_LEFT || direction == DH_DIRECTION_RIGHT)
                              ? center_x(candidate) - center_x(from)
                              : center_y(candidate) - center_y(from);
    return (uint64_t)(delta < 0 ? -delta : delta);
}

bool dh_place_target(const dh_place *place, const dh_display_rect *displays, size_t count,
                     size_t primary_index, dh_place_point *point) {
    if (place == NULL || displays == NULL || point == NULL || count == 0 ||
        primary_index >= count || place->chain_index == 0)
        return false;

    size_t current = primary_index;
    for (uint8_t screen = 1; screen < place->chain_index; screen++) {
        size_t next = count;
        uint64_t best = UINT64_MAX;
        for (size_t i = 0; i < count; i++) {
            if (i == current || !lies_in_direction(&displays[current], &displays[i],
                                                   place->chain_direction))
                continue;
            const uint64_t distance = directional_distance(
                &displays[current], &displays[i], place->chain_direction);
            if (distance < best) {
                best = distance;
                next = i;
            }
        }
        if (next == count)
            return false;
        current = next;
    }

    const dh_display_rect *target = &displays[current];
    if (target->width <= 0 || target->height <= 0)
        return false;
    const int32_t x_along = (int32_t)(((uint64_t)place->entry_position *
                                       (uint32_t)(target->width - 1) + 32767u) / 65535u);
    const int32_t y_along = (int32_t)(((uint64_t)place->entry_position *
                                       (uint32_t)(target->height - 1) + 32767u) / 65535u);
    point->display_index = current;
    switch (place->border_direction) {
        case DH_DIRECTION_LEFT:
            point->x = target->x; point->y = target->y + y_along; break;
        case DH_DIRECTION_RIGHT:
            point->x = target->x + target->width - 1; point->y = target->y + y_along; break;
        case DH_DIRECTION_TOP:
            point->x = target->x + x_along; point->y = target->y; break;
        case DH_DIRECTION_BOTTOM:
            point->x = target->x + x_along; point->y = target->y + target->height - 1; break;
        default: return false;
    }
    return true;
}
