/*
 * How a measure-only image types its answer (#110, #166).
 *
 * NOT part of the product. The bench images have no console: both stdio paths
 * are off (CMakeLists.txt) and the UART pins carry the inter-board link. The
 * board is a keyboard already, so the answer is typed as keystrokes into
 * whatever has focus. One line, built with bench_append_* and then typed one key
 * event per bench_type_step call.
 *
 * Header-only and static: exactly one bench file is compiled per image, so
 * the state below has one owner.
 */
#pragma once

#include "main.h"

static char bench_line[256]; /* the UART line is 190 characters at its widest */
static size_t bench_line_len;
static size_t bench_typed;        /* index of the next character */
static bool bench_key_is_down;    /* every character is a press then a release */

/* Digits, lower-case letters and space. Everything a bench prints is one of
   those, so it types the same on any keyboard layout. */
static uint8_t bench_hid_key_for(char c) {
    if (c >= 'a' && c <= 'z')
        return (uint8_t)(HID_KEY_A + (c - 'a'));
    if (c == '0')
        return HID_KEY_0;
    if (c > '0' && c <= '9')
        return (uint8_t)(HID_KEY_1 + (c - '1'));
    return HID_KEY_SPACE;
}

static void bench_append_text(const char *text) {
    while (*text != '\0' && bench_line_len + 1u < sizeof bench_line)
        bench_line[bench_line_len++] = *text++;
}

static void bench_append_u32(uint32_t value) {
    char digits[11];
    size_t n = 0;
    do {
        digits[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value > 0u && n < sizeof digits);
    while (n > 0u && bench_line_len + 1u < sizeof bench_line)
        bench_line[bench_line_len++] = digits[--n];
}

/*
 * Queue one key event -- a press or a release -- and say whether the line is
 * finished. One per call, never a batch: the keyboard queue is short and
 * queue_try_add drops silently when it is full, so filling it would lose
 * characters out of the middle of the answer. Call it at about 100 Hz.
 */
static bool bench_type_step(device_t *state) {
    if (bench_typed >= bench_line_len)
        return true;
    hid_keyboard_report_t report = {0};
    if (!bench_key_is_down)
        report.keycode[0] = bench_hid_key_for(bench_line[bench_typed]);
    queue_kbd_report(&report, state);
    if (bench_key_is_down)
        bench_typed++;
    bench_key_is_down = !bench_key_is_down;
    return false;
}
