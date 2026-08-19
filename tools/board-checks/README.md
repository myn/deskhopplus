# Board checks

Measurements that can only be taken on real hardware. Same idea as
`tools/macos-checks/` and `tools/windows-checks/`, one layer down: these need a
board, a cable, and a person.

## `ecdh-timing` — how long a P-256 ECDH really takes (#110)

[ADR-0008](../../docs/adr/0008-channel-identity-and-sealed-clipboard.md) estimates
80–200 ms on this chip and says plainly that the number is unmeasured. It matters
because core 0 runs six jobs in one cooperative loop with no preemption, including
the keyboard and mouse queues at 2000 Hz (`src/main.c`), and the watchdog budget is
500 ms (`src/include/watchdog.h`). An 80 ms cost and a 400 ms cost are different
designs, and [#111](https://github.com/myn/deskhopplus/issues/111) is written from
whichever it is.

### What the image does

Ten seconds after the host enumerates it, the board generates one key pair, runs
ten ECDHs, and **types the answer as keystrokes** into whatever has focus:

```
deskhopplus ecdh keygen 41233 us mean 38122 us worst 38940 us over 10 runs ok
```

It is a keyboard already, and this is cheaper than the alternatives: both stdio
paths are off in this firmware, the UART pins carry the inter-board link, and
adding a CDC interface would change the USB descriptor set — a bigger change to
the thing being measured than the measurement is worth.

One ECDH runs per pass of the main loop, never a batch, so the loop keeps
reaching `kick_watchdog_task` and the 2000 Hz queues in between. The watchdog is
kicked immediately before each measured call, so the whole 500 ms budget is
available to it.

### Running it

```sh
cmake -S . -B build-bench -DDH_BENCH_ECDH=ON
cmake --build build-bench -j"$(sysctl -n hw.ncpu)"
```

1. **Flash `build-bench/deskhop.uf2` onto board A** — BOOTSEL, then drag it onto
   the mounted volume. Board A, because it is the one attached to the Mac.
2. Open a text editor and put the cursor in it.
3. Wait about ten seconds. The line types itself.
4. **Flash the real firmware back**: `./tools/build.sh fw`, then
   `build/deskhop.uf2` the same way.

### What it will not do

**It does not announce itself to the peer board.** The heartbeat carries this
board's firmware version and checksum, and the far side pulls the image whenever
it differs — including on an *equal* version carrying different bytes
([#91](https://github.com/myn/deskhopplus/issues/91)). This image is exactly
that, so left announcing it would flash itself onto board B, and getting the
real firmware back onto B costs a cable trip. `src/main.c` drops
`heartbeat_output_task` from core 1 in this build for that reason. You do not
need to unplug anything.

### If it reboots in a loop instead of typing

That is the result, not a bug: one ECDH did not fit inside the 500 ms watchdog
window. Say so on #110 and on
[#95](https://github.com/myn/deskhopplus/issues/95) — #111 cannot then run it
inline on core 0, and the design has to move before it is built.

### What this image is not

It is not the firmware. It types into whatever has focus, it tells the peer
board nothing, and it does no pairing. Do not leave it on a board.
