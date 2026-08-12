# ADR-0006: The Windows helper is a single portable exe that installs nothing

- **Status:** Accepted
- **Date:** 2026-08-12
- **Arising from:** [#49](https://github.com/myn/deskhopplus/issues/49)
- **Prior art:** [mkroamer ADR-0003](https://github.com/myn/mkroamer/blob/main/docs/adr/0003-toolchains.md), whose Windows half this adopts

## Decision

The Windows helper ships as **one `.exe` that requires nothing installed on the target machine** —
no VC++ redistributable, no runtime, no installer, no administrator step. The user downloads it,
puts it anywhere, and runs it.

The rule that governs future choices is not the flag, it is this: **no dependency may require
anything installed on the target machine.** Concretely, today:

1. **Static CRT.** `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>` (`/MT`), so
   there is no redistributable to install.
2. **Inbox libraries only** — `user32 shell32 ole32 setupapi hid cfgmgr32 crypt32`. Anything not
   already on a stock Windows install is not a candidate.
3. **No package manager.** No vcpkg, no conan. Third-party code, if it ever becomes necessary, is
   vendored header-only, so a clean MSVC box builds the tree as checked out.
4. **Embedded manifest**: PerMonitorV2 DPI awareness and `asInvoker` — the helper never requests
   elevation.
5. **MSVC, C++20 for the helper; C11 for `src/core`, compiled in place.** The core is compiled as C,
   under the same static runtime, never routed through the C++ compiler — see Consequences.
6. **CI publishes the exe** as a build artifact on every push to `main`, the same shape as the
   firmware's `.uf2`. That is what makes the property real rather than aspirational: the deliverable
   a user receives is the thing this ADR describes.

## Context

Every measurement behind [ADR-0001](0001-vendor-hid-transport.md) was taken on a **managed Windows 11
Enterprise laptop, unelevated and unsigned** — that machine is the target, not a convenience. The
transport was chosen, and its evidence gathered, specifically to avoid needing permissions the user
does not have. A helper that then asks for a redistributable install gives back at the last step
exactly what the transport decision spent its evidence buying.

The same user solved this in `mkroamer` and recorded it there; this ADR adopts that half rather than
rediscovering it.

## Alternatives considered

| Option | Why not |
| --- | --- |
| **Dynamic CRT + VC++ redistributable** | The redist installer is the administrator step ADR-0001 exists to avoid. It also fails silently in the worst way: the exe runs on the developer's box and refuses to start on the target. |
| **vcpkg/conan for dependencies** | Both make "what does this need installed" a question with a per-machine answer. The helper's dependency surface is small enough that the constraint costs nothing today, and the constraint is the point. |
| **An MSI or other installer** | Nothing needs installing. An installer would exist only to place a file, while adding a policy surface a managed laptop can refuse. |
| **MinGW cross-build on the Linux runner** | Cheaper CI, but an odd fit for a program that is mostly Win32, and it would compile `src/core` with a fourth toolchain while testing it under none of the ones that ship. |

## Consequences

- **`src/core` gains a third compiler.** It is C11 and has only ever been read by gcc and clang;
  MSVC at `/W4` is the strictest of the three. Warnings it raises are treated as core defects to fix,
  not to suppress — a third independent reading of code that also runs on the microcontroller is
  worth having. Compiling those C sources through the C++ compiler instead would have made the
  Windows build compile something subtly different from what the firmware ships, defeating the
  reason the core is shared at all ([#64](https://github.com/myn/deskhopplus/issues/64)).
- **The host test harness (`tests/`) carries the same runtime setting**, in its own `CMakeLists.txt`
  rather than as a flag passed by CI. A rule that lives only in a YAML file is invisible to anyone
  building locally on a Windows box, which is the drift this ADR is trying to prevent.
- **A portable exe moves.** Autostart entries therefore register the helper's *own* current location
  (`GetModuleFileNameW`) and rewrite themselves when it no longer matches, rather than assuming an
  install path. `mkroamer`'s `autostart.h` chose the Startup folder for exactly this reason; #49's
  three-mechanism ladder keeps that self-heal and adds the fallbacks a managed laptop needs.
- **The secret cannot lean on an install location or a code signature.** DPAPI
  (`CryptProtectData`, user-account-bound, no signature, no prompt) is what remains, and it is a
  better fit here than the macOS helper's plain file — see `SecretStore.swift`, whose Keychain
  reasoning does not transfer.
