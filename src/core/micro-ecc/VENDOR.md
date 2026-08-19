# micro-ecc, vendored

Upstream: <https://github.com/kmackay/micro-ecc>
Commit: `541b3a78026420a3e369c4c9281c396b5e531113` (2024-11-13)
Licence: BSD 2-clause — `LICENSE.txt`, GPLv3-compatible, which is what ADR-0008 and
[#110](https://github.com/myn/deskhopplus/issues/110) required of the P-256 dependency.

## Why in the tree

Same reason `pico-sdk/` and `Pico-PIO-USB/` are: this repository has no package manager on
any of its three toolchains, and the firmware build must not reach the network. It is
vendored, not submoduled, so a clone is a build.

## What was left out

`asm_avr.inc` and `asm_avr_mult_square.inc` (960 KB, AVR only — `uECC.c` includes them
only under `uECC_PLATFORM == uECC_avr`, which no target here is), and upstream's
`test/`, `examples/`, `scripts/` and build files. Nothing else is modified: **do not patch
these files.** Configuration is set in `../dh_p256.c`, which is the only file that
compiles `uECC.c`.

## Updating

Copy the same file list from a newer upstream commit, update the hash above, then run the
host suite: `auth_test` gates P-256 against `test-vectors/primitives.txt`, so a change in
behaviour fails there rather than in the field.
