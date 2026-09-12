/*
 * A measure-only build: how long one P-256 ECDH actually takes on this chip.
 *
 * NOT part of the product. Compiled only when the firmware is configured with
 * -DDH_BENCH_ECDH=ON, and the resulting image is a measuring instrument, not
 * something to leave on a board — it types into whatever has focus and it
 * does nothing else useful.
 *
 * Why it exists: ADR-0008 estimates a P-256 ECDH at 80-200 ms on this chip and
 * says plainly that the number is unmeasured, and #110 requires the real one
 * before #111 designs around it. Core 0 runs six jobs in one cooperative loop
 * with no preemption, including the keyboard and mouse queues at 2000 Hz
 * (src/main.c), and the watchdog budget is 500 ms (src/include/watchdog.h).
 * An 80 ms cost and a 400 ms cost are different designs.
 *
 * The answer is typed as keystrokes because the board is already a keyboard.
 * Both stdio paths are off in this firmware (CMakeLists.txt), the UART pins
 * carry the inter-board link, and adding a CDC interface would change the USB
 * descriptor set — which is a bigger change to the thing being measured than
 * the measurement is worth.
 *
 * Usage is in tools/board-checks/README.md.
 */

#include "main.h"

#ifdef DH_BENCH_ECDH

#include <hardware/watchdog.h>
#include <pico/time.h>

#include "bench_typing.h"
#include "dh_p256.h"

#define BENCH_RUNS 10u
#define BENCH_SETTLE_US (10u * 1000u * 1000u) /* let the host enumerate, and the user pick a window */

/* Obviously fake, and fixed so the run is repeatable. Entropy is the caller's
   job (#110) and this caller has none to offer. */
static const uint8_t bench_private[DH_P256_PRIVATE_SIZE] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20};
static const uint8_t bench_peer_private[DH_P256_PRIVATE_SIZE] = {
    0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30,
    0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40};

enum bench_phase {
    BENCH_SETTLING,
    BENCH_KEYGEN,
    BENCH_ECDH,
    BENCH_TYPING,
    BENCH_DONE,
};

static enum bench_phase phase = BENCH_SETTLING;
static uint8_t peer_public[DH_P256_PUBLIC_SIZE];
static uint32_t keygen_us;
static uint32_t ecdh_total_us;
static uint32_t ecdh_worst_us;
static unsigned runs_done;
static bool arithmetic_ok = true;

/*
 * The watchdog is kicked immediately before each measured call, so the whole
 * 500 ms budget is available to it and a reset means the operation really did
 * exceed the budget rather than that time had already been spent elsewhere in
 * the loop. If this build reboots in a loop instead of typing, that IS the
 * result: one ECDH does not fit inside the watchdog window, and #111 cannot
 * run it inline on core 0.
 */
static uint32_t timed_ecdh(const uint8_t *private_key, const uint8_t *public_key, uint8_t *out) {
    watchdog_update();
    uint32_t started = time_us_32();
    if (!dh_p256_ecdh(private_key, public_key, out))
        arithmetic_ok = false;
    return time_us_32() - started;
}

void bench_ecdh_task(device_t *state) {
    switch (phase) {
        case BENCH_SETTLING:
            /* Nothing may be typed before the host is there to receive it, and
               the user needs a moment to put the cursor somewhere harmless. */
            if (state->tud_connected && time_us_64() > BENCH_SETTLE_US)
                phase = BENCH_KEYGEN;
            return;

        case BENCH_KEYGEN: {
            /* First boot generates the board's identity, so this is measured
               too — it is the same scalar multiply and it happens once. */
            watchdog_update();
            uint32_t started = time_us_32();
            if (!dh_p256_public_from_private(bench_peer_private, peer_public))
                arithmetic_ok = false;
            keygen_us = time_us_32() - started;
            phase = BENCH_ECDH;
            return;
        }

        case BENCH_ECDH: {
            /* One per pass, never a batch: the loop has to get back to
               kick_watchdog_task and to the 2000 Hz queues between runs, which
               is also the only way the board stays usable while measuring. */
            uint8_t shared[DH_P256_SHARED_SIZE];
            uint32_t elapsed = timed_ecdh(bench_private, peer_public, shared);
            ecdh_total_us += elapsed;
            if (elapsed > ecdh_worst_us)
                ecdh_worst_us = elapsed;

            if (++runs_done < BENCH_RUNS)
                return;

            bench_append_text("deskhopplus ecdh keygen ");
            bench_append_u32(keygen_us);
            bench_append_text(" us mean ");
            bench_append_u32(ecdh_total_us / BENCH_RUNS);
            bench_append_text(" us worst ");
            bench_append_u32(ecdh_worst_us);
            bench_append_text(" us over ");
            bench_append_u32(BENCH_RUNS);
            bench_append_text(arithmetic_ok ? " runs ok" : " runs failed");
            phase = BENCH_TYPING;
            return;
        }

        case BENCH_TYPING:
            if (bench_type_step(state))
                phase = BENCH_DONE;
            return;

        case BENCH_DONE:
        default:
            return;
    }
}

#endif /* DH_BENCH_ECDH */
