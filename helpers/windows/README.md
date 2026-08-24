# The Windows helper

One `.exe`. Put it anywhere and run it. Nothing is installed, nothing is elevated, and it is
unsigned — see [ADR-0006](../../docs/adr/0006-windows-helper-no-install.md) for why that is a
requirement rather than a convenience.

## What it does

Finds the board by its USB identifier and serial, takes exclusive hold of every channel, says
hello, keeps the session alive, and reconnects on its own when the device goes away and comes
back. It shows what it is doing in the notification area, and can be asked to start itself at
logon.

**It decides none of that.** The session machine is `src/core/dh_helper.c`, compiled in place —
the same code the firmware ships and the macOS helper drives. This helper is a transport and a
face: `hid_transport.cpp` carries reports, `words.cpp` carries wording, and everything between
them is `helper_session.cpp` handing events across.

## Building

Needs MSVC and CMake. No vcpkg, no conan, no toolchain file — a clean box builds the tree as
checked out.

```
cmake -S helpers/windows -B helpers/windows/build
cmake --build helpers/windows/build --config Release
ctest --test-dir helpers/windows/build -C Release
```

The exe lands at `helpers/windows/build/Release/deskhop-helper.exe`. CI builds and publishes it
on every push to `main`, which is what makes the no-install property something a user receives
rather than something this file asserts.

The tests cover the autostart ladder's decisions and nothing else. That is deliberate: the ladder
is the code most likely to be wrong on a managed laptop nobody can reproduce, and it needs no
registry to be worth checking. The Win32 calls behind it, and the transport, are verified by hand
and on hardware (#87) — the same line the macOS helper draws.

## Pairing

Start the helper **first**, then press the config chord on the board. Three facts, each of which
has cost a debugging session:

- the chord is a **toggle** — two quick presses enter and immediately leave, which looks exactly
  like the chord not working;
- the pairing window is **per board**, and the secret is written to the flash of whichever board
  processed the chord — and the board that processes it is the one **the keyboard is plugged
  into**, so pairing this helper means moving the keyboard to board B's USB-A port for the two
  presses;
- the window is 60 seconds, and a helper started after it opens has missed it.

## Where it keeps things

`%LOCALAPPDATA%\deskhopplus\`:

| File | What it is |
| --- | --- |
| `identity` | this helper's P-256 private key, DPAPI-protected |
| `board_key` | the board's public key, pinned at pairing, DPAPI-protected |
| `autostart` | which autostart mechanism took, and whether it has been seen to fire |
| `helper.log` | what the helper has been doing |

**What DPAPI here does and does not protect.** It binds the blobs to this Windows account on this
machine, so copying them elsewhere yields nothing. It does **not** defend against a process
running as the same user: anything with this account's token can unprotect them and read the
private key. There is no Secure Enclave equivalent on Windows, and the macOS helper's key really
is non-extractable where this one is not. The mitigation that applies is rotation — a fresh chord
press registers a new key and the old one stops being accepted. `secret_store.h` says the same
thing at more length, and says why there is deliberately no optional entropy.

## Autostart

Off until you turn it on from the tray menu. When you do, three mechanisms are tried in order — a
logon task, a run-key entry, a shortcut in the Startup folder — and the first that takes is
recorded. A managed laptop can refuse any of them, and all three failing is logged and nothing
more: the helper is an enhancement, never a dependency.

"Enabled" is only claimed once two different things are true: the entry reads back, **and** a
launch carrying the entry's own argument has actually been seen. Those are different claims, and
a policy that leaves a run key sitting there while refusing to act on it satisfies the first and
not the second. The tray menu says which of the two you have.

The entry names wherever the exe currently is, and is rewritten when that stops matching. A
portable exe moves, and this is what stops that silently breaking autostart.

## Removing it

Turn autostart off from the tray menu, quit the helper, delete
`%LOCALAPPDATA%\deskhopplus\`, and delete the exe. There is nothing else.
