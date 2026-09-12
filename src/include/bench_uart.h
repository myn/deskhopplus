/*
 * What the UART bench (#166) sends, and how it counts what it receives.
 *
 * NOT part of the product. src/bench_uart.c is the measure-only image, built
 * with -DDH_BENCH_UART=ON; this header is the part of it with no SDK in it,
 * so tests/bench_uart_test.c can reach the counting the same way
 * txq_test.c reaches dh_txq.h. Usage is in tools/board-checks/README.md.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "packet.h"

/*
 * The bench's packet type. Outside enum packet_type_e on purpose: the product
 * firmware has no handler for it, so a board still on the real image ignores
 * the hellos while it pulls this one, and process_packet never has to know
 * the bench exists.
 */
#define BENCH_UART_MSG 0xB0u

/*
 * How much to send. Payload bytes, since that is the unit #39 measured the
 * USB hop in: 250000 packets is 2,000,000 bytes, the size of #39's long sets,
 * and the short stamp lands at 25000 packets -- 200,000 bytes, #39's shortest
 * set. The same run therefore answers whether the rate holds with size.
 */
#define BENCH_UART_PACKETS 250000u
#define BENCH_UART_SHORT   25000u

/*
 * Payload rule: data32[0] is the sequence number, data32[1] its complement.
 * Self-checking, so the receiver needs no table and no total. seq 0 is the
 * hello -- "I am running the bench" -- and the flood runs from 1.
 *
 * A byte XORed with its complement is 0xFF, so the wire's 8-bit XOR checksum
 * (calc_checksum) is 0 for every one of these payloads and a flip of the
 * same bit in a byte and its mirror passes it. The 32-bit rule does not, and
 * that is what the `bad` count is: corruption the wire check let through.
 */
static inline uint32_t bench_uart_check(uint32_t seq) {
    return ~seq;
}

typedef struct {
    uint32_t intact;   /* flood packets whose payload obeyed the rule */
    uint32_t bad;      /* passed the wire checksum, failed the rule */
    uint32_t highest;  /* highest seq seen: highest - intact is what never arrived */
    uint32_t first_us; /* arrival of the first flood packet */
    uint32_t last_us;  /* arrival of the latest one, intact or not */
    uint32_t short_us; /* arrival of the BENCH_UART_SHORT-th intact one; 0 until then */
} bench_uart_rx_t;

/* One bench packet arrived and passed the wire checksum. A hello counts as
   nothing; everything else moves the window and highest, and is then either
   intact or bad. Volatile because on the board core 1 writes this and core 0
   reads it; a plain struct converts to it for free. */
static inline void bench_uart_rx_note(volatile bench_uart_rx_t *rx, uint32_t seq, uint32_t check,
                                      uint32_t now_us) {
    if (seq == 0)
        return;
    if (rx->highest == 0)
        rx->first_us = now_us;
    rx->last_us = now_us;
    if (seq > rx->highest)
        rx->highest = seq;
    if (check != bench_uart_check(seq)) {
        rx->bad++;
        return;
    }
    if (++rx->intact == BENCH_UART_SHORT)
        rx->short_us = now_us;
}

/* Payload bytes per second over a window. 64-bit in the middle: the long
   window's packets * 8 * 1000000 is 2e12, well past 32 bits. */
static inline uint32_t bench_uart_bytes_per_s(uint32_t packets, uint32_t elapsed_us) {
    if (elapsed_us == 0)
        return 0;
    return (uint32_t)((uint64_t)packets * PACKET_DATA_LENGTH * 1000000u / elapsed_us);
}
