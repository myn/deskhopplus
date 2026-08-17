#!/usr/bin/env bash
#
# #92 criterion 2: can an interrupted pull leave a part-written image running?
#
#   sudo ./tools/macos-checks/interrupted_pull_to_rom.sh check
#   sudo ./tools/macos-checks/interrupted_pull_to_rom.sh run
#
# It flashes board A older than board B so A becomes the receiver, waits for
# the pull to start, has you unplug B, and times the landing. `peer_fw` goes
# stale after 3 s and the stall lands 30 s after the last progress, so a board
# that is going to ROM should be there by about 33 s.
#
# THIS ONE IS DESTRUCTIVE BY DESIGN. It ends with board A in the ROM
# bootloader, which is the pass. Recovery is one picotool load, and the script
# prints it.
#
# The reason it is a script and not a checklist is the evidence. Two paths
# reach the ROM bootloader and they mean opposite things:
#
#   recover_to_rom   erases the first sector       the image was dirty and the
#                                                  peer had gone — the pass
#   reset_usb_boot   erases nothing, image intact  the chord, or a peer message
#
# `picotool save` before reflashing is the only thing that tells them apart,
# and a `picotool load` destroys it. That has already been lost once. So the
# save happens here, automatically, before anything else — and the script reads
# the first sector back and says which path ran.
#
# Needs root. Run it from a real terminal: the `!` prefix in a Claude Code
# session has no TTY for the sudo password prompt.

set -uo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo"

OLD="${DH_OLD:-$HOME/deskhop-0.90.uf2}"        # goes on A, must be older than B
NEW="${DH_NEW:-$repo/build/deskhop.uf2}"       # what A is restored to at the end
DEADLINE="${DH_DEADLINE:-120}"                 # give up this long after the unplug
POLL=0.5

# PEER_FW_STALE_US is 3 s and FW_UPGRADE_STALL_US is 30 s.
EXPECT_BY=45

LOG="${DH_LOG:-$HOME/deskhop-pull-run-$(date +%Y%m%d-%H%M%S).log}"
SAVED="${DH_SAVED:-$HOME/deskhop-rom-$(date +%Y%m%d-%H%M%S).uf2}"

bold=$'\033[1m'; red=$'\033[31m'; green=$'\033[32m'; yellow=$'\033[33m'; off=$'\033[0m'

# Shares its reasoning with config_timeout_with_stall.sh: pair idVendor with
# idProduct inside each device block, and use `-p IOUSB` rather than the
# whole-registry plist, which costs ~2 s a sample.
state() {
    ioreg -p IOUSB -l -w 0 2>/dev/null | awk '
        /\+-o /                  { if (v && p) ids[v ":" p] = 1; v=""; p="" }
        /"idVendor" *= *[0-9]+/  { v=$3 }
        /"idProduct" *= *[0-9]+/ { p=$3 }
        END {
            if (v && p) ids[v ":" p] = 1
            if ("11914:3"    in ids) { print "rom";    exit }
            if ("11914:4220" in ids) { print "config"; exit }
            if ("4617:49152" in ids) { print "normal"; exit }
            print "none"
        }'
}

now()   { python3 -c 'import time; print("%.2f" % time.time())'; }
since() { python3 -c "print('%.1f' % ($(now) - $1))"; }
say()   { printf '%s  %s\n' "$(date '+%H:%M:%S')" "$*" | tee -a "$LOG"; }

# The version out of the .uf2 itself, not a sidecar .crc and not CMakeLists.txt.
# `misc/crc32.py` writes {magic 0xf00d, version u16, crc u32} into
# .section_metadata, so it can be found in the payload wherever the linker put
# it — which means an artifact that has been moved or renamed still answers for
# itself. Prints "<version> <crc>" for the caller to split.
version_of() {
    python3 - "$1" <<'PY'
import struct, sys

payload = {}
with open(sys.argv[1], 'rb') as fh:
    while (b := fh.read(512)):
        if len(b) < 512:
            break
        m0, m1, flags, addr, size, no, total, fam = struct.unpack('<8I', b[:32])
        if m0 == 0x0A324655 and m1 == 0x9E5D5157:
            payload[addr] = b[32:32 + size]

flat = b"".join(payload[a] for a in sorted(payload))
at = flat.find(b"\x0d\xf0\x00\x00")
if at == -1:
    print("0 0x0")
else:
    magic, version, crc = struct.unpack('<IHI', flat[at:at + 10])
    print(f"{version} {crc:#010x}")
PY
}

wait_for() {
    local want="$1" limit="$2" start; start="$(now)"
    while [ "$(state)" != "$want" ]; do
        if [ "$(python3 -c "print(1 if $(now) - $start > $limit else 0)")" = "1" ]; then
            return 1
        fi
        sleep "$POLL"
    done
}

preflight() {
    local ok=0
    [ "$(id -u)" -eq 0 ] || { printf '%s\n' "${red}not root — rerun with sudo${off}"; ok=1; }

    local ov="" nv=""
    for f in "$OLD" "$NEW"; do
        if [ -f "$f" ]; then
            local v c; read -r v c <<<"$(version_of "$f")"
            printf '%-22s version %s  crc %s\n' "$(basename "$f")" "$v" "$c"
            [ "$f" = "$OLD" ] && ov="$v" || nv="$v"
        else
            printf '%s\n' "${red}missing $f${off}"; ok=1
        fi
    done

    # A board pulls only from a strictly newer peer, so an OLD that is not
    # actually older leaves board A sitting there and the criterion untested —
    # the same silent nothing that #91 is about.
    if [ -n "$ov" ] && [ -n "$nv" ] && [ "$ov" -ge "$nv" ]; then
        printf '%s\n' "${red}$(basename "$OLD") is not older than $(basename "$NEW") — A would never pull${off}"; ok=1
    fi

    command -v picotool >/dev/null || { printf '%s\n' "${red}no picotool — brew install picotool${off}"; ok=1; }

    local s; s="$(state)"
    printf 'board A    %s\n' "$s"
    [ "$s" = "normal" ] || { printf '%s\n' "${red}board A must be running normally to take the chord${off}"; ok=1; }

    printf 'evidence   %s\n' "$SAVED"
    printf 'expect     ROM within ~%s s of the unplug\n' "$EXPECT_BY"
    [ "$ok" -eq 0 ] && printf '%s\n' "${green}ready — this ends with board A in ROM${off}"
    return "$ok"
}

run() {
    preflight || return 1
    : >"$LOG"

    # ---- board A down to the older build, so B's heartbeat outranks it
    printf '\n%s\n' "${bold}Press Left Shift + Right Shift + A on the DeskHop keyboard.${off}"
    if ! wait_for rom 120; then
        say "${red}board A never reached BOOTSEL — nothing was changed${off}"; return 1
    fi
    say "board A in BOOTSEL"

    if ! picotool load -x "$OLD" >>"$LOG" 2>&1; then
        say "${red}picotool load failed — see $LOG${off}"; return 1
    fi
    say "flashed $(basename "$OLD")"

    if ! wait_for normal 60; then
        say "${red}board A did not come back — recover with: picotool load -x $NEW${off}"; return 1
    fi
    say "board A running the older build; it should now be pulling from B"

    # The pull is past its first page within ~16 ms, so by the time this line
    # prints the image is already dirty. The wait is for the operator, not the
    # firmware.
    sleep 5

    printf '\n%s\n' "${bold}Unplug board B now, and leave it unplugged.${off}"
    printf '%s\n\n' "Board B is only the sender here — its own image is not touched."
    read -r -p "Press Return once B is out: " _ </dev/tty
    local U; U="$(now)"
    say "keypress (u=0) — NOT the unplug: board B is on the other machine, so"
    say "the hand that pulls the cable cannot also be on the keyboard. Measured"
    say "at 11 s of lag on 2026-08-17. Read u+ as a lower bound on the interval."

    # ---- the landing
    while :; do
        local s; s="$(state)" ; local e; e="$(since "$U")"
        if [ "$s" = "rom" ]; then
            # The absolute time is the trustworthy half: the stall counts 30 s
            # back from here to the last accepted response, which is the real
            # unplug. u+ carries the operator's lag and is only a lower bound.
            say "${green}${bold}ROM${off} at $(date '+%H:%M:%S') — u+${e} s, so the last response landed ~$(date -v-30S '+%H:%M:%S')"
            break
        fi
        if [ "$(python3 -c "print(1 if $e > $DEADLINE else 0)")" = "1" ]; then
            say "${red}${bold}still running at u+${e} s${off} — no ROM landing"
            say "A board left running a part-written image is criterion 2 FAILING."
            say "Check what it is running before touching it:  picotool save -r 0x10000000 0x10040000 $SAVED"
            return 3
        fi
        sleep "$POLL"
    done

    # ---- evidence first, always, before any recovery
    if picotool save -r 0x10000000 0x10040000 "$SAVED" >>"$LOG" 2>&1; then
        say "saved flash to $SAVED"
    else
        say "${red}picotool save FAILED — do not reflash yet, the evidence is still on the board${off}"
        return 4
    fi

    local verdict
    verdict="$(python3 - "$SAVED" <<'PY'
# Two questions of the saved flash, and the second is the one that is easy to
# lose.
#
# Which road to ROM: recover_to_rom erases the first 4096-byte sector and
# nothing else, reset_usb_boot erases nothing.
#
# And whether the pull was really interrupted. A readback cannot see a
# *completed* pull at all — one binary serves both boards, so it rewrites every
# page with identical bytes and leaves no trace (#104). An interrupted one is
# different: write_flash_page erases a whole sector to write into it, so the
# pages past the frontier are still erased. That ragged edge is the part-written
# image this criterion exists to catch, and printing where it lies is what makes
# the run evidence rather than an anecdote.
import struct, sys

BASE = 0x10000000
blocks = {}
with open(sys.argv[1], 'rb') as fh:
    while (b := fh.read(512)):
        if len(b) < 512:
            break
        m0, m1, flags, addr, size, no, total, fam = struct.unpack('<8I', b[:32])
        if m0 == 0x0A324655 and m1 == 0x9E5D5157:
            blocks[addr] = b[32:32 + size]

erased = sorted((a - BASE) // 256 for a, d in blocks.items() if all(x == 0xFF for x in d))
runs = []
for pg in erased:
    if runs and pg == runs[-1][1] + 1:
        runs[-1][1] = pg
    else:
        runs.append([pg, pg])

sector0 = [r for r in runs if r[0] == 0 and r[1] >= 15]
if not blocks:
    print("unknown | saved image unreadable")
elif sector0:
    later = [r for r in runs if r[0] > 15]
    if later:
        lo, hi = later[0]
        pulled = lo * 256
        where = (f"pull stopped mid-sector at page {lo - 1} ({BASE + (lo - 1) * 256:#010x}); "
                 f"pages {lo}-{hi} still erased; {pulled} of {1024 * 256} bytes "
                 f"({100 * pulled / (1024 * 256):.1f}%)")
    else:
        where = "no frontier past sector 0 — the pull may have completed; check the metadata version"
    print(f"recover_to_rom | first sector erased (all 0xff) | {where}")
else:
    print("reset_usb_boot | first sector intact — the image was NOT wiped |")
PY
)"
    IFS='|' read -r road detail frontier <<<"$verdict"
    say "evidence: ${bold}${road}${off} —${detail}"
    [ -n "${frontier// /}" ] && say "frontier:${frontier}"

    case "$verdict" in
        recover_to_rom*)
            say "${green}${bold}PASS${off} — the interrupted pull wiped stage 2 rather than leaving a part-written image running" ;;
        reset_usb_boot*)
            say "${yellow}the board reached ROM without recover_to_rom running — that is a different path, not this criterion${off}" ;;
        *)
            say "${yellow}could not read the first sector back; keep $SAVED${off}" ;;
    esac

    printf '\n%s\n' "${bold}To put board A back:${off}"
    printf '  picotool load -x %s\n' "$NEW"
    printf '  then plug board B back in\n'
}

case "${1:-run}" in
    check) preflight ;;
    run)   run; rc=$?; printf '\nlog: %s\n' "$LOG"; exit "$rc" ;;
    *)     printf 'usage: %s [check|run]\n' "$0"; exit 2 ;;
esac
