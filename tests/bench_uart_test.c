/*
 * The UART bench's receive accounting and rate arithmetic (#166).
 *
 * The board types one line and a wrong number in it costs a flash of both
 * boards to notice. So the counting lives in a header with no SDK in it
 * (src/include/bench_uart.h), and this checks it the way config_test.c checks
 * config_store.c: a named assertion macro, a main, a printed failure line, a
 * non-zero exit -- no framework.
 */

#include <stdio.h>

#include "bench_uart.h"

static int failures = 0;

#define CHECK(cond, name, what)                                                 \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++failures;                                                         \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, (name), (what)); \
        }                                                                       \
    } while (0)

/* An intact flood packet: the sender's own payload rule. */
static void arrive(bench_uart_rx_t *rx, uint32_t seq, uint32_t now_us) {
    bench_uart_rx_note(rx, seq, bench_uart_check(seq), now_us);
}

int main(void) {
    /* The hello (seq 0) says the peer is up. It is not payload. */
    {
        bench_uart_rx_t rx = {0};
        arrive(&rx, 0, 1000);
        CHECK(rx.intact == 0 && rx.bad == 0 && rx.highest == 0, "hello", "hello was counted");
        CHECK(rx.first_us == 0, "hello", "hello started the window");
    }

    /* Three in order: all intact, window from first to last arrival. */
    {
        bench_uart_rx_t rx = {0};
        arrive(&rx, 1, 1000);
        arrive(&rx, 2, 1040);
        arrive(&rx, 3, 1080);
        CHECK(rx.intact == 3, "order", "intact count");
        CHECK(rx.highest == 3, "order", "highest seq");
        CHECK(rx.bad == 0, "order", "nothing was bad");
        CHECK(rx.first_us == 1000 && rx.last_us == 1080, "order", "window bounds");
    }

    /* A gap shows as highest running ahead of intact: seq 3 and 4 never came. */
    {
        bench_uart_rx_t rx = {0};
        arrive(&rx, 1, 1000);
        arrive(&rx, 2, 1040);
        arrive(&rx, 5, 1160);
        CHECK(rx.intact == 3, "gap", "intact count");
        CHECK(rx.highest == 5, "gap", "highest seq");
        CHECK(rx.highest - rx.intact == 2, "gap", "two lost");
    }

    /* A payload that passed the wire checksum but not the payload rule is bad,
       not intact -- and still moves highest and the window. */
    {
        bench_uart_rx_t rx = {0};
        arrive(&rx, 1, 1000);
        bench_uart_rx_note(&rx, 2, 0xdeadbeef, 1040);
        CHECK(rx.intact == 1, "bad", "corrupt packet counted as intact");
        CHECK(rx.bad == 1, "bad", "corrupt packet not counted");
        CHECK(rx.highest == 2, "bad", "corrupt packet did not move highest");
        CHECK(rx.last_us == 1040, "bad", "corrupt packet did not move the window");
    }

    /* The short-window stamp lands on the SHORT-th *intact* packet. A bad one
       in the middle pushes it one arrival later. */
    {
        bench_uart_rx_t rx = {0};
        uint32_t now = 1000;
        for (uint32_t seq = 1; seq < BENCH_UART_SHORT; ++seq, now += 40)
            arrive(&rx, seq, now);
        CHECK(rx.short_us == 0, "short", "stamped before the short window filled");

        bench_uart_rx_note(&rx, BENCH_UART_SHORT, 0, now); /* bad */
        CHECK(rx.short_us == 0, "short", "a bad packet filled the short window");

        arrive(&rx, BENCH_UART_SHORT + 1, now + 40);
        CHECK(rx.short_us == now + 40, "short", "stamp is not the SHORT-th intact arrival");

        arrive(&rx, BENCH_UART_SHORT + 2, now + 80);
        CHECK(rx.short_us == now + 40, "short", "stamp moved after the window filled");
    }

    /* Payload bytes per second. 250000 packets of 8 bytes in ten seconds is
       2,000,000 bytes in 10 s: 200,000 bytes/s -- and the product overflows
       32 bits on the way, which is the case the arithmetic must survive. */
    {
        CHECK(bench_uart_bytes_per_s(250000, 10000000) == 200000, "rate", "long window");
        CHECK(bench_uart_bytes_per_s(25000, 1000000) == 200000, "rate", "short window");
        CHECK(bench_uart_bytes_per_s(1, 40) == 200000, "rate", "one packet");
        CHECK(bench_uart_bytes_per_s(100, 0) == 0, "rate", "empty window divides by zero");
    }

    if (failures == 0)
        printf("bench_uart_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
