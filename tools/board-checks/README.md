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

## `uart-throughput` — what the inter-board link really carries (#166)

Every figure for the inter-board link is arithmetic: 3686400 baud (`src/include/serial.h`)
and 8 payload bytes per 12 wire bytes give ~200 KB/s, and
[ADR-0002](../../docs/adr/0002-parallel-hid-channels.md) reasons from that number
in three places. Nobody has observed it.
[#39](https://github.com/myn/deskhopplus/issues/39) measured the whole path at
33 KB/s and found the USB hop is the wall, so the UART has never been under
load.

### What the image does

**Both boards run it.** Ten seconds after its host enumerates it, each board
sends one hello a second across the inter-board link until a bench packet
arrives from the other side — proof the peer board is on this image too. Then
it floods: 250,000 numbered packets (2,000,000 payload bytes, the size of #39's
long sets) as fast as `uart_tx_queue` drains, while counting the peer board's
flood as it arrives on core 1 through `process_packet`, checksum and all. Two
seconds after the last packet from either side, each board **types one line**
into whatever has focus on its own computer:

```
deskhopplus uart tx 250000 pkt 10021845 us 199564 bytes per s short 199712 rx 250000 pkt 10023120 us 199538 bytes per s short 199690 highest 250000 bad 0
```

Reading it, left to right:

- `tx … pkt … us … bytes per s` — what this board sent: packets the queue
  accepted, the window from first accept to last, and payload bytes/s over it.
  The queue is 256 deep and the window counts its first fill as instant, so
  this reads about 0.1% high.
- `short` — the same rate over the first 25,000 packets (200,000 bytes, #39's
  shortest set). If it matches the long one, the rate holds with size.
- `rx … pkt … us … bytes per s short` — the same four for what arrived from
  the peer board and passed both the wire checksum and the payload rule. This is the
  intact ceiling, and the number to compare with #39's 33 KB/s.
- `highest` — the highest sequence number seen. `highest` minus `rx pkt` is how
  many packets never arrived at all: an overrun of the 1024-byte receive ring
  or a failed wire checksum, which `process_packet` drops silently. Core 1
  takes one packet per pass of its loop, so a large gap with `bad 0` means the
  board could not drain the ring at the wire rate — the receive ceiling is the
  board's, not the wire's, and `tx` is then the better reading of the wire.
- `bad` — packets that passed the wire checksum and still carried the wrong
  payload. A ceiling that includes corruption is not a ceiling.

Board A types on the Mac and board B on the Windows machine, so both directions
are read without a cable trip. Both floods overlap: the inter-board link is full duplex,
transmit is core 0's DMA and receive is core 1's ring, so neither takes from
the other.

### Running it

```sh
cmake -S . -B build-bench -DDH_BENCH_UART=ON
cmake --build build-bench -j"$(sysctl -n hw.ncpu)"
```

1. **Flash `build-bench/deskhop.uf2` onto board A** — BOOTSEL, then drag it
   onto the mounted volume.
2. **Wait for board B to pull it.** Unlike `ecdh-timing`, this image keeps its
   heartbeat, so B sees a different image at the same version and pulls it
   over the inter-board link ([#91](https://github.com/myn/deskhopplus/issues/91)) — the
   only way to reach B, whose Windows machine denies writes to the config
   volume. B reboots into it when the pull completes. A's hellos are ignored
   by the product firmware and are too sparse to slow the pull.
3. Put the cursor in a text editor on **both** computers. The flood starts on
   its own once both boards are up; it takes about ten seconds at the
   arithmetic rate, longer if the inter-board link is slower.
4. Two lines type themselves, one per computer. Paste both into #166, saying
   which board typed which.
5. **Flash the real firmware back**: `./tools/build.sh fw`, then
   `build/deskhop.uf2` onto board A the same way. B pulls it for the same
   reason it pulled the bench.

Then run it once more with the mouse moving the whole time: unplug and replug
both boards — the image runs once and then sits in its done state — put the
cursor back in the editors, and keep the mouse moving from the moment the
flood starts. The image relays input as the product does, so the second pair
of lines is the rate with input in the path, and any stutter is visible at the
desk. Read it knowing the flood is harsher than the relay ever is: it keeps
`uart_tx_queue` within 32 slots of full, so every mouse packet waits behind up
to ~224 others (~7 ms) — delayed, not dropped, which is what the 32 slots are
for. [ADR-0005](../../docs/adr/0005-bounded-outbound-queues.md)'s queues are
the product's answer to the same contention at the relay's real rate.

### What it will do to board B

It propagates, and a failed pull is the risk any firmware release carries: a
checksum mismatch at the end erases B's stage-2 bootloader and leaves it in ROM
recovery, which is the cable trip, only worse. Nothing about this image makes
that more likely than a product build does, but it is one more pull than the
measurement strictly needs, and it is the reason to run this on a day the
cable is within reach.

### What this image is not

It is not the firmware. It is the product image with a flood and a typist
bolted on: input, the helper channel and the heartbeat all still run, which is
what makes the mouse-moving run meaningful, and it types into whatever has
focus. Do not leave it on either board.
