/*
 * deskhopplus — where a config Read All has got to. See
 * include/config_read_all.h for why this is not simply a loop in the handler.
 */

#include "config_read_all.h"

void config_read_all_start(config_read_all_t *walk, uint16_t field_count) {
    walk->count   = field_count;
    walk->pending = field_count;
}

bool config_read_all_peek(const config_read_all_t *walk, uint16_t *index) {
    if (walk->pending == 0)
        return false;

    *index = (uint16_t)(walk->count - walk->pending);
    return true;
}

void config_read_all_sent(config_read_all_t *walk, uint16_t index) {
    uint16_t current;

    if (!config_read_all_peek(walk, &current) || current != index)
        return;

    walk->pending--;
}

void config_read_all_stop(config_read_all_t *walk) {
    walk->pending = 0;
}
