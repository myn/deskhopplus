/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Derek Reynolds */

/*
 * When the Status LED goes dark on its own (#283).
 *
 * The board asks this on every pass of status_led_task (led.c); the answer
 * only matters while this board's computer is the active output, because the
 * LED is dark otherwise anyway. Pure C11, no clock read of its own: the caller
 * passes one `now` (the #107 rule).
 */

#ifndef DH_STATUS_LED_H_
#define DH_STATUS_LED_H_

#include <stdbool.h>
#include <stdint.h>

/* config_t.led_off_mode. Zero is Never, so a config written before #283 keeps
   the LED lit. */
enum {
    DH_STATUS_LED_NEVER        = 0,
    DH_STATUS_LED_IDLE         = 1, /* no input for the time, counted from the later of input and switch */
    DH_STATUS_LED_AFTER_SWITCH = 2, /* the time after a switch, even while typing */
    DH_STATUS_LED_OFF          = 3, /* aways turn off */
};

/* The page's default After time (form.py, field 102), and what a stored zero
   loads as. */
#define DH_STATUS_LED_SEC_DEFAULT 60u

/* True when the Status LED should be dark. Times are microseconds from one
   clock, compared signed, so a stamp taken just after `now` reads as recent. */
static inline bool dh_status_led_dark(uint8_t mode, uint16_t seconds, uint64_t now_us,
                                      uint64_t last_input_us, uint64_t last_switch_us,
                                      bool config_mode) {
    if (config_mode || seconds == 0)
        return false;
    
    if (mode == DH_STATUS_LED_OFF)
        return true;
   
    uint64_t since;
    if (mode == DH_STATUS_LED_IDLE)
        since = (int64_t)(last_input_us - last_switch_us) > 0 ? last_input_us : last_switch_us;
    else if (mode == DH_STATUS_LED_AFTER_SWITCH)
        since = last_switch_us;
    else
        return false;

    return (int64_t)(now_us - since) >= (int64_t)seconds * 1000000;
}

#endif /* DH_STATUS_LED_H_ */
