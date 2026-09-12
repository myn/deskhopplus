/*
 * A measure-only build: what the inter-board UART actually carries (#166).
 *
 * NOT part of the product. Compiled only when the firmware is configured with
 * -DDH_BENCH_UART=ON, and the resulting image is a measuring instrument, not
 * something to leave on a board — it types into whatever has focus and it
 * floods the inter-board link with nonsense.
 *
 * Why it exists: every statement about the inter-board link is arithmetic. 3686400 baud
 * and 8 payload bytes per 12 wire bytes give ~200 KB/s, ADR-0002 reasons
 * from that figure in three places, and nobody has observed it. #39 measured
 * the whole path at 33 KB/s and found the USB hop is the wall, so the UART
 * has never been under load.
 *
 * Both boards run this image. Each one:
 *
 *   1. settles, then sends one hello a second until a bench packet arrives
 *      from the peer board — which is how it learns the peer board is on this
 *      image too, and why the flood cannot start while it is still pulling it;
 *   2. floods: keeps uart_tx_queue topped up with BENCH_UART_PACKETS numbered
 *      packets, as fast as the queue drains;
 *   3. counts what arrives from the peer board's flood, on core 1, in the handler
 *      that process_packet already dispatches to — so the receive path under
 *      test is the product's own, checksum and all;
 *   4. waits for both floods to finish, then types one line. Board B is a
 *      keyboard on the Windows machine, so both halves can be read without a
 *      cable trip.
 *
 * The two floods overlap. The inter-board link is full duplex, transmit is core 0's DMA
 * and receive is core 1's ring, so neither direction takes from the other,
 * and both boards talking at once is the condition the relay runs in.
 *
 * Usage is in tools/board-checks/README.md.
 */

#include "main.h"

#ifdef DH_BENCH_UART

#include <pico/time.h>

#include "bench_typing.h"
#include "bench_uart.h"

#define BENCH_SETTLE_US (10u * 1000u * 1000u) /* let the host enumerate, and the user pick a window */
#define BENCH_HELLO_US  (1000u * 1000u)
#define BENCH_QUIET_US  (2u * 1000u * 1000u)  /* no packet for this long: the peer board's flood is over */
#define BENCH_KEY_PASSES 10u                  /* one key event per this many 1 kHz passes */
#define BENCH_HEADROOM   32u                  /* queue slots left for input: one ms of drain */

enum bench_phase {
    BENCH_SETTLING,
    BENCH_HELLO,
    BENCH_FLOOD,
    BENCH_DRAIN,
    BENCH_TYPING,
    BENCH_DONE,
};

static enum bench_phase phase = BENCH_SETTLING;

/* Written on core 1 by bench_uart_rx_msg, read on core 0. Every read is a
   single aligned word, which is atomic on this chip, and the line is built
   only after BENCH_QUIET_US of silence — by which time nothing is writing. */
static volatile bench_uart_rx_t rx;
static volatile bool peer_board_up;

static uint32_t sent;          /* flood packets the queue accepted, 0..BENCH_UART_PACKETS */
static uint32_t tx_first_us;   /* first accept */
static uint32_t tx_short_us;   /* the BENCH_UART_SHORT-th accept */
static uint32_t tx_last_us;    /* the last accept */
static uint32_t last_hello_us;
static unsigned key_passes;

static bool send_bench_packet(uint32_t seq, device_t *state) {
    uart_packet_t packet = {.type = BENCH_UART_MSG};
    packet.data32[0] = seq;
    packet.data32[1] = bench_uart_check(seq);
    return queue_uart_packet(&packet, state);
}

/* Core 1, from process_packet: the wire checksum has already passed. */
void bench_uart_rx_msg(uart_packet_t *packet, device_t *state) {
    (void)state;
    peer_board_up = true;
    bench_uart_rx_note(&rx, packet->data32[0], packet->data32[1], time_us_32());
}

static void build_line(void) {
    uint32_t tx_us = tx_last_us - tx_first_us;
    uint32_t rx_us = rx.last_us - rx.first_us;
    bench_append_text("deskhopplus uart tx ");
    bench_append_u32(sent);
    bench_append_text(" pkt ");
    bench_append_u32(tx_us);
    bench_append_text(" us ");
    bench_append_u32(bench_uart_bytes_per_s(sent, tx_us));
    bench_append_text(" bytes per s short ");
    bench_append_u32(bench_uart_bytes_per_s(BENCH_UART_SHORT, tx_short_us - tx_first_us));
    bench_append_text(" rx ");
    bench_append_u32(rx.intact);
    bench_append_text(" pkt ");
    bench_append_u32(rx_us);
    bench_append_text(" us ");
    bench_append_u32(bench_uart_bytes_per_s(rx.intact, rx_us));
    bench_append_text(" bytes per s short ");
    bench_append_u32(rx.intact >= BENCH_UART_SHORT
                         ? bench_uart_bytes_per_s(BENCH_UART_SHORT, rx.short_us - rx.first_us)
                         : 0);
    bench_append_text(" highest ");
    bench_append_u32(rx.highest);
    bench_append_text(" bad ");
    bench_append_u32(rx.bad);
}

void bench_uart_task(device_t *state) {
    uint32_t now = time_us_32();

    switch (phase) {
        case BENCH_SETTLING:
            /* Nothing may be typed before the host is there to receive it, and
               the user needs a moment to put the cursor somewhere harmless. */
            if (state->tud_connected && time_us_64() > BENCH_SETTLE_US)
                phase = BENCH_HELLO;
            return;

        case BENCH_HELLO:
            /* One packet a second is nothing to a peer board still pulling
               this image over the same inter-board link; a flood would drown
               its requests. */
            if (peer_board_up) {
                phase = BENCH_FLOOD;
                return;
            }
            if (now - last_hello_us >= BENCH_HELLO_US) {
                send_bench_packet(0, state);
                last_hello_us = now;
            }
            return;

        case BENCH_FLOOD:
            /* Top the queue up rather than push until refused: a refusal is a
               counted drop (#43) and there is nothing to learn from a million
               of them. The queue is 256 deep and drains at a packet every
               ~33 us, so at 1 kHz it never runs dry between passes — and the
               last BENCH_HEADROOM slots are left for core 1's input packets,
               which mouse.c throws away when refused. Input still waits
               behind the flood, up to ~7 ms, but it is not lost.

               The window runs from first accept to last accept, which counts
               the initial fill as instant: 0.1% high on the long window, 1%
               on the short one. The receive side has no such offset, and it
               is the number that matters. */
            while (sent < BENCH_UART_PACKETS
                   && queue_get_level(&state->uart_tx_queue) < UART_QUEUE_LENGTH - BENCH_HEADROOM) {
                /* Core 1 shares the queue, so a refusal is still possible
                   between the check and the add; a packet the queue refused
                   was never sent, and counting it would show as a gap. */
                if (!send_bench_packet(sent + 1, state))
                    break;
                sent++;
                if (sent == 1)
                    tx_first_us = now;
                if (sent == BENCH_UART_SHORT)
                    tx_short_us = now;
            }
            if (sent == BENCH_UART_PACKETS) {
                tx_last_us = now;
                phase = BENCH_DRAIN;
            }
            return;

        case BENCH_DRAIN:
            /* Our flood is queued; wait for the peer board's to stop arriving.
               A peer board that never floods leaves last_us at zero, and zero
               is quiet too. Signed, because core 1 can stamp last_us from a
               clock read after this pass took `now` — unsigned, that wraps
               to "quiet for 71 minutes" (#107). */
            if ((int32_t)(now - rx.last_us) > (int32_t)BENCH_QUIET_US
                && now - tx_last_us > BENCH_QUIET_US) {
                build_line();
                phase = BENCH_TYPING;
            }
            return;

        case BENCH_TYPING:
            if (++key_passes % BENCH_KEY_PASSES != 0)
                return;
            if (bench_type_step(state))
                phase = BENCH_DONE;
            return;

        case BENCH_DONE:
        default:
            return;
    }
}

#endif /* DH_BENCH_UART */
