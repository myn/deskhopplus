# The Windows helper

Using it — pairing, what the tray says, the clipboard, fixing and removing it — is the
[user guide](../../docs/user-guide.md). This file is for people building it.

One `.exe`. Put it anywhere and run it. Nothing is installed, nothing is elevated, and it is
unsigned — see [ADR-0006](../../docs/adr/0006-windows-helper-no-install.md) for why that is a
requirement rather than a convenience.

## What it does

Finds the board by its USB identifier and serial, takes exclusive hold of every channel, says
hello, keeps the session alive, and reconnects on its own when the device goes away and comes
back. It carries the clipboard — text, images and files — and places the cursor. It shows what it
is doing in the notification area, and can be asked to start itself at logon. The tray menu's
first row, greyed, is **deskhopplus helper** and the release number, from the one version the
firmware and both helpers share (`src/core/dh_version.h`). It is not clickable.

The icon is always there, with one of three looks — `words::look`: **paired**, the solid glyph,
for connected, including a live config-mode session; **off**, the outlined glyph, for looking,
absent and config mode without a session; **attention**,
the badge, for every state with a remedy, the reconnect rate, and a waiting file question — and
the state words in the tooltip (`words::tooltip`), which a look never replaces (#38). While a
file arrives it is the percent, two white digits on a blue tile drawn with GDI+ at the taskbar's
DPI (`Tray::digits`). The looks are `.ico` resources (`src/deskhop-helper.rc`, rendered by
`helpers/icon/render.sh` from the Mac menu bar's glyph);
the first of them, `IDI_APP`, is what Explorer shows for the exe.

Windows 11 puts a new icon behind the taskbar's **^** overflow. After the icon appears the helper
sets `IsPromoted` on its own record under `HKCU\Control Panel\NotifyIconSettings` — undocumented,
per-user, keyed by exe path — so it sits on the taskbar; it does so on every start, so a moved exe
heals itself. If Windows ignores it, the one-time fallback is **Settings › Personalization ›
Taskbar › Other system tray icons › deskhopplus helper › On** (#208).

Files arriving from the other computer are **offered, not pushed**
([ADR-0011](../../docs/adr/0011-paste-side-acceptance-starts-a-file-transfer.md)): a set over
1 MB waits in the notification area until it is accepted here, and only then does anything cross
the link. They are written under `%TEMP%\deskhopplus`, which is emptied when the helper starts.

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

The tests cover the autostart ladder, channel discovery, the clipboard path, the seal's
cipher, the shim's dispatch — which output reaches which effect — and what the presence shows
for a state (`words_test`: the look and the tooltip, #208). The ladder is the code most
likely to be wrong on a managed laptop nobody can reproduce, and it needs no registry to be worth
checking. The dispatch is the layer a service can emit the right output into and have nothing
happen ([#152](https://github.com/myn/deskhopplus/issues/152)), which is #93 and #94's shape; it
needs no device and no
Win32 to run, because the tray, the transport, the clipboard and the secret store sit behind an
interface a test implements — so it can be run from a developer's own machine as well as from CI. The seal test needs Windows because CNG is Windows — and that is
exactly why it is here rather than in the shared suite: nobody else's machine can check that
**CNG's** AES-256-GCM produces the bytes on the wire. The Win32 calls themselves — the ladder's,
the transport's, the tray's — are verified by hand and on hardware (#87), the same line the macOS
helper draws.

## Pairing

Start the helper **first**, then press the config chord on the board. The three facts that have
each cost a debugging session — the chord is a toggle, the window is per board and opens only on
the board the keyboard is plugged into, and it lasts 60 seconds — are in the user guide's
[Pair a helper](../../docs/user-guide.md#pair-a-helper).

## The clipboard payload is sealed

Bulk payloads are encrypted **helper to helper**
([ADR-0008](../../docs/adr/0008-channel-identity-and-sealed-clipboard.md),
[#113](https://github.com/myn/deskhopplus/issues/113)). Both boards relay ciphertext and hold no
key that opens it, so what a board forwards is bytes it could not read even if it wanted to.

The key is agreed per seal over ephemeral P-256 keys, and the cipher is CNG's AES-256-GCM —
`BCryptEncrypt` in GCM chaining mode, from `bcrypt.dll`, which is already on this helper's list of
inbox libraries. ChaCha20-Poly1305 would have needed Windows 11 and so was never a candidate here.

The DPAPI-held identity below is **not** part of the seal: a seal needs no long-term identity, and
its keys do not outlive the session. Cursor placement is authenticated but not sealed — the
coordinates cross in the clear, by decision.

No build turns this off. #44's development-build exemption is the *board's*, and the board is not
a party to this key.

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

The tick on **Start at logon** means you asked for it. Whether it actually fired is a different
claim — a policy can leave a run key sitting there while refusing to act on it — and that proof
is in the log: `autostart confirmed` is written the first time a launch carrying the entry's own
argument is seen.

The entry names wherever the exe currently is, and is rewritten when that stops matching. A
portable exe moves, and this is what stops that silently breaking autostart.

## Removing it

The user guide's [Uninstall](../../docs/user-guide.md#uninstall). There is nothing else to remove.
