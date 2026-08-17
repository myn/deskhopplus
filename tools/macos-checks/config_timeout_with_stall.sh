#!/usr/bin/env bash
#
# #92 criterion 1: does config mode end within ~330 s while a transfer is
# stalled in flight?
#
#   sudo ./tools/macos-checks/config_timeout_with_stall.sh check
#   sudo ./tools/macos-checks/config_timeout_with_stall.sh run
#
# `check` is a dry preflight and touches nothing. `run` waits for the config
# chord, then drives the whole induction on a clock: mount, drop 16 UF2 blocks
# at t+280 s, unmount, and watch USB identity until the board leaves.
#
# It exists because the run cannot be driven by hand. Three things make that so:
#
#   The config page must never be opened. Every GET/SET ends in
#   reset_config_timer (src/handlers.c), so one glance at the page pushes the
#   deadline out by another 300 s and the run measures the wrong clock.
#
#   The keyboard is dead while config mode is active — it belongs to the board
#   under test — so nothing can be typed between the chord and the result.
#   Everything after the chord has to already be running.
#
#   A live mount pins the departed config-mode device in the IORegistry, so
#   ioreg reports config mode for a board that has already left. Unmounting the
#   moment the drop lands settles that, and is safe: a non-UF2 write returns at
#   the magic check (src/ramdisk.c) before fw_upgrade_progress, so the FAT
#   metadata flushed on unmount cannot reset the stall window.
#
# Needs root for mount/umount. Run it from a real terminal: the `!` prefix in a
# Claude Code session has no TTY for the sudo password prompt.

set -uo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo"

# ------------------------------------------------------------------ tunables

IMAGE="${DH_IMAGE:-$repo/build/deskhop.uf2}"   # must be what board A is RUNNING
MOUNTPOINT="${DH_MOUNT:-/Volumes/DESKHOP}"
DROP_AT="${DH_DROP_AT:-280}"                   # seconds after config entry
BLOCKS="${DH_BLOCKS:-16}"                      # 16 = one whole 4096-byte sector
DEADLINE="${DH_DEADLINE:-420}"                 # give up this long after entry
POLL=0.5

# CONFIG_MODE_TIMEOUT is 300 s and FW_UPGRADE_STALL_US is 30 s, so a drop at
# t+280 should stall at t+310 and the deferred reboot should follow at once.
PASS_BY=330

LOG="${DH_LOG:-$HOME/deskhop-stall-run-$(date +%Y%m%d-%H%M%S).log}"

bold=$'\033[1m'; red=$'\033[31m'; green=$'\033[32m'; yellow=$'\033[33m'; off=$'\033[0m'

# ------------------------------------------------------------------ plumbing

# Where board A is, as one token: normal, config, rom, or none.
#
# idVendor and idProduct do not appear in a fixed order inside a device block,
# so they are paired per block rather than grepped for separately — a
# line-oriented match pairs them across devices and reads a state that is not
# there. `-p IOUSB` rather than `-r -c IOUSBHostDevice -a`: the plist form of
# the whole registry takes ~2 s to produce and parse, which is a poor
# instrument for a run measured against a 330 s deadline.
#
#   0x1209/0xc000  DeskHop Switch, running firmware
#   0x2e8a/0x107c  config mode, with the UF2 disk
#   0x2e8a/0x0003  RPI-RP2, the ROM bootloader
#
# ROM beats config beats normal: if the board has landed in ROM that is the
# answer, whatever a ghost mount is still holding open.
state() {
    ioreg -p IOUSB -l -w 0 2>/dev/null | awk '
        /\+-o /                        { if (v && p) ids[v ":" p] = 1; v=""; p="" }
        /"idVendor" *= *[0-9]+/        { v=$3 }
        /"idProduct" *= *[0-9]+/       { p=$3 }
        END {
            if (v && p) ids[v ":" p] = 1
            if ("11914:3"     in ids) { print "rom";    exit }
            if ("11914:4220"  in ids) { print "config"; exit }
            if ("4617:49152"  in ids) { print "normal"; exit }
            print "none"
        }'
}
now()   { python3 -c 'import time; print("%.2f" % time.time())'; }
since() { python3 -c "print('%.1f' % ($(now) - $1))"; }
stamp() { date '+%H:%M:%S'; }

say() {
    printf '%s  %s\n' "$(stamp)" "$*" | tee -a "$LOG"
}

disk_nodes() { ls /dev/disk[0-9]* 2>/dev/null | grep -E '/dev/disk[0-9]+$' | sort; }

cleanup_mount() {
    if mount | grep -q " on $MOUNTPOINT "; then
        say "unmounting $MOUNTPOINT"
        umount -f "$MOUNTPOINT" >>"$LOG" 2>&1 || say "${yellow}umount failed — unplug the board to clear the ghost${off}"
    fi
}

# ---------------------------------------------------------------- preflight

preflight() {
    local ok=0

    [ "$(id -u)" -eq 0 ] || { printf '%s\n' "${red}not root — rerun with sudo${off}"; ok=1; }

    if [ -f "$IMAGE" ]; then
        printf 'image      %s (%s bytes)\n' "$IMAGE" "$(stat -f '%z' "$IMAGE")"
        # Read the version out of the artifact's metadata, not CMakeLists.txt,
        # so it reports what board A must actually be running for the drop to
        # write identical bytes.
        if [ -f "${IMAGE%.uf2}.crc" ]; then
            printf 'version    %s\n' "$(python3 - "${IMAGE%.uf2}.crc" <<'PY'
import struct, sys
magic, version, crc = struct.unpack('<IHI', open(sys.argv[1], 'rb').read()[:10])
print(f"{version}  (v{version // 1000}.{version % 1000 - 100}, crc {crc:#010x})")
PY
)"
        fi
    else
        printf '%s\n' "${red}no image at $IMAGE — run ./tools/build.sh${off}"; ok=1
    fi

    local s; s="$(state)"
    printf 'board A    %s\n' "$s"
    case "$s" in
        normal) ;;
        config) printf '%s\n' "${yellow}already in config mode — let it time out first${off}"; ok=1 ;;
        rom)    printf '%s\n' "${red}in ROM — recover with picotool load -x first${off}"; ok=1 ;;
        *)      printf '%s\n' "${red}not found — is board A plugged into this Mac?${off}"; ok=1 ;;
    esac

    if mount | grep -q " on $MOUNTPOINT "; then
        printf '%s\n' "${red}$MOUNTPOINT is already mounted — a stale mount pins a ghost. umount -f it${off}"; ok=1
    fi
    [ -d "$MOUNTPOINT" ] || { mkdir -p "$MOUNTPOINT" && printf 'created    %s\n' "$MOUNTPOINT"; }

    printf 'disks now  %s\n' "$(disk_nodes | tr '\n' ' ')"
    printf 'drop       %s blocks of the running image at t+%s s\n' "$BLOCKS" "$DROP_AT"
    printf 'pass       config mode ends by t+%s s\n' "$PASS_BY"

    if [ "$ok" -eq 0 ]; then
        printf '%s\n' "${green}ready${off}"
    fi
    return "$ok"
}

# --------------------------------------------------------------------- run

run() {
    preflight || return 1
    : >"$LOG"

    local before; before="$(disk_nodes)"

    printf '\n%s\n' "${bold}Press the config chord on the DeskHop keyboard now: Left Ctrl + Right Shift + C + O${off}"
    printf '%s\n\n' "Do not open the config page at any point. Nothing else is needed from you."

    # ---- wait for config mode
    local waited=0
    while [ "$(state)" != "config" ]; do
        sleep "$POLL"
        waited=$((waited + 1))
        if [ "$waited" -gt 600 ]; then          # 5 minutes of nothing
            say "${red}no config mode after 5 min — aborting, nothing was written${off}"
            return 1
        fi
    done

    local T; T="$(now)"
    say "${green}config mode entered${off}  (t=0)"

    # ---- find and mount the UF2 disk
    local node="" tries=0
    while [ -z "$node" ] && [ "$tries" -lt 60 ]; do
        node="$(comm -13 <(printf '%s\n' "$before") <(disk_nodes) | head -1)"
        [ -n "$node" ] && break
        sleep "$POLL"; tries=$((tries + 1))
    done

    if [ -z "$node" ]; then
        say "${red}no new disk node appeared — aborting before the drop${off}"
        return 1
    fi
    say "disk node $node  (t+$(since "$T") s)"

    if ! mount -t msdos "$node" "$MOUNTPOINT" >>"$LOG" 2>&1; then
        say "${red}mount failed — aborting before the drop${off}"
        return 1
    fi
    say "mounted $MOUNTPOINT  (t+$(since "$T") s)"
    trap 'cleanup_mount' EXIT

    # ---- wait until the drop moment
    while [ "$(python3 -c "print(1 if $(now) - $T < $DROP_AT else 0)")" = "1" ]; do
        if [ "$(state)" != "config" ]; then
            say "${red}board left config mode before the drop — nothing was written${off}"
            return 1
        fi
        sleep 1
    done

    # ---- the drop: whole sectors of the board's OWN image, so identical bytes
    say "${bold}dropping $BLOCKS blocks${off}  (t+$(since "$T") s)"
    if dd if="$IMAGE" of="$MOUNTPOINT/part.uf2" bs=512 count="$BLOCKS" >>"$LOG" 2>&1; then
        sync
        local D; D="$(now)"
        say "${green}drop written and synced${off}  (t+$(since "$T") s) — stall due at t+$(python3 -c "print('%.1f' % ($D - $T + 30))") s"
    else
        say "${red}dd failed — see $LOG${off}"
        return 1
    fi

    # ---- unmount at once, so USB identity can be trusted from here on
    cleanup_mount
    
    say "unmounted  (t+$(since "$T") s)"

    # ---- watch for the exit
    local s
    while :; do
        s="$(state)"
        local elapsed; elapsed="$(since "$T")"

        case "$s" in
            normal)
                say "${green}left config mode at t+${elapsed} s${off}"
                if [ "$(python3 -c "print(1 if $elapsed <= $PASS_BY else 0)")" = "1" ]; then
                    say "${green}${bold}PASS${off} — criterion 1 met (within $PASS_BY s)"
                else
                    say "${yellow}${bold}LATE${off} — it exited, but after $PASS_BY s"
                fi
                return 0 ;;
            rom)
                say "${red}${bold}ROM at t+${elapsed} s${off} — recover_to_rom ran"
                say "The peer check is wrong, and it is now provable: the drop's own"
                say "checksum branch needs block 1023, and the pull cannot run with"
                say "source=DROP, so image_dirty && !peer_present is the only road left."
                say "${bold}Take the evidence BEFORE recovering:${off}"
                say "  sudo picotool save -r 0x10000000 0x10040000 ~/deskhop-rom-\$(date +%H%M).uf2"
                say "  sudo picotool load -x $IMAGE"
                return 2 ;;
        esac

        if [ "$(python3 -c "print(1 if $elapsed > $DEADLINE else 0)")" = "1" ]; then
            say "${red}${bold}still in config mode at t+${elapsed} s${off} — no exit, no ROM"
            say "That is the #90 defect returning: a stalled transfer holding config mode open."
            return 3
        fi
        sleep "$POLL"
    done
}

# -------------------------------------------------------------------- main

case "${1:-run}" in
    check) preflight ;;
    run)   run; rc=$?; printf '\nlog: %s\n' "$LOG"; exit "$rc" ;;
    *)     printf 'usage: %s [check|run]\n' "$0"; exit 2 ;;
esac
