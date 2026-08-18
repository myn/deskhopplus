# ADR-0007: The inter-board link is trusted for firmware

- **Status:** Accepted
- **Date:** 2026-08-17
- **Resolves:** [#62](https://github.com/myn/deskhopplus/issues/62)
- **Unblocks:** [#91](https://github.com/myn/deskhopplus/issues/91), which ships unconditionally as a result

## Decision

A board accepts firmware from its peer board **unconditionally**. No config mode, no physical
chord, no authenticated request. The exposure this creates — a compromised board can flash the
other one — is **accepted and documented**, not constrained.

[#91](https://github.com/myn/deskhopplus/issues/91) therefore ships in release builds, not behind
`DH_DEBUG` or a config flag. The gate it would have hidden behind does not exist, because this
decision is that there should not be one.

## Context

[#62](https://github.com/myn/deskhopplus/issues/62) records finding F1 from
[`chris-010/deskhop`](https://github.com/chris-010/deskhop): a compromised board can flash its
peer over the inter-board link, and one 12-byte packet is enough. Verified against this tree on
2026-08-17. Two mechanisms, both live, both accepted with no precondition whatever:

| Packet | Handler | Effect on the peer board |
|---|---|---|
| `HEARTBEAT_MSG` carrying a high version | `src/handlers.c`, `handle_heartbeat_msg` | Pulls the sender's entire image and writes it over its own running image |
| `FIRMWARE_UPGRADE_MSG` | `src/handlers.c`, `handle_fw_upgrade_msg` | Reboots straight into BOOTSEL |

`process_packet` (`src/uart.c`) verifies the XOR checksum and dispatches. That checksum is an
integrity check against line noise; it is not a signature and there is nothing secret in it. No
handler on the firmware path asks whether the board is in config mode, whether a chord is held,
or whether anything was requested.

The mechanism is not merely present in the source — it is **measured working end to end**. Board
B has reached 0.83, 0.89 and 0.92 by pulling from board A over the inter-board link, with the
receiving board rebooting itself unaided.

## Alternatives considered

**Require config mode, a chord, or an authenticated request.** Rejected on three grounds.

*It does not close the hole.* Every gate that could be built stops a subset of the packets. A
compromised board that cannot use the equal-version path simply claims a higher version and uses
the strictly-newer one; a board that cannot do either sends `FIRMWARE_UPGRADE_MSG` and puts its
peer in BOOTSEL. Closing it properly means authenticating the link, which means a shared secret,
which means provisioning and key storage across two MCUs that have neither today. That is a large
build, and the next point is why it would not be worth it.

*The isolation it would defend is not load-bearing against a compromised board.* An attacker with
code execution on either board already sits between the keyboard and both computers, and can
inject keystrokes into either of them at will. That is the device's function. Reflashing the peer
board gains such an attacker nothing they do not already have, so the boundary being protected
does not separate anything.

*It breaks the one workflow that keeps board B flashable.* Board B's computer blocks USB writes,
so the peer pull is how B receives firmware; flashing it directly costs a physical cable trip to
the other machine ([#58](https://github.com/myn/deskhopplus/issues/58)). A gate on the pull turns
every firmware change into that trip.

**The containment argument this once supported is already void.**
[ADR-0003](0003-content-fidelity-over-content-validation.md) records that there is no egress-board
validator, so nothing anywhere now reasons from "the peer board is hostile". #62 predicted this
would happen, and it has: the finding stands purely as a firmware-security question, and this is
its answer.

## Consequences

- The threat model says plainly that **compromise of one board is compromise of the pair**. Any
  future security argument that treats the two boards as separate trust domains is wrong, and
  should be checked against this ADR before it is written down.
- [#91](https://github.com/myn/deskhopplus/issues/91)'s equal-version CRC sync ships
  unconditionally. Its `BOARD_ROLE == OUTPUT_B` tiebreaker stays, but it is a **correctness**
  control and not a security one: it stops two boards that disagree at one version from each
  pulling the other. It should never be described as limiting the exposure, because a compromised
  board B can still reach A through the strictly-newer path, which is unchanged.
- **A hand flash of board B no longer sticks on its own.** At equal version B pulls board A's
  image back over anything flashed directly onto it, silently, on the next heartbeat. That
  follows from this decision rather than from #91 alone: an ungated pull is a pull that always
  wins. The remedy is to flash **A**, which is cheaper than flashing B was — so the change makes
  recovery shorter, but it inverts the instinct, and the run sheets say so at both the version
  section and the ROM-recovery section.
- Physical access to the inter-board link is equivalent to firmware write access on both boards.
  The link is a short trace or jumper between two boards in one enclosure, which is what makes
  that acceptable rather than merely true.
- If the boards ever gain an authenticated inter-board channel for another reason, firmware
  transfer should move onto it. This ADR accepts an exposure given the cost of removing it; it
  does not argue the exposure is desirable.
