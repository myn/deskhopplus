#!/usr/bin/env bash
#
# The clipboard's hardware run sheet (#56), because the last one was done by
# hand and written up by hand, and both halves of that are avoidable.
#
#   ./tools/clipboard-run-sheet.sh files     make the fixtures, once
#   ./tools/clipboard-run-sheet.sh mark      draw a line under the logs, before a sitting
#   ./tools/clipboard-run-sheet.sh sheet     print the steps to work through
#   ./tools/clipboard-run-sheet.sh watch     follow both logs and say what happens
#   ./tools/clipboard-run-sheet.sh report    what the logs say about the last run
#   ./tools/clipboard-run-sheet.sh throughput how fast each set actually moved
#   ./tools/clipboard-run-sheet.sh selftest   check the timing reader itself
#
# `throughput` is the answer to #39. It times each set from the moment the
# receiving helper asked for it to the moment it arrived whole, so the number
# comes out of the same log the sitting already produces — no stopwatch, and
# no arithmetic done by hand afterwards.
#
# `watch` is the one that matters. It reads both helpers' logs at once and
# turns them into one ordered line per event, so a sitting produces its own
# write-up. It also names the failures that are already known, because
# recognising one on the spot is the difference between finishing the sheet and
# spending the evening on a bug someone already filed.
#
# It reads logs and writes fixtures. It never touches the device, the config or
# either helper — every gesture in the sheet is yours, because every one of
# them is a thing only a person at two computers can do.

set -uo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

MAC_LOG="${MAC_LOG:-/tmp/deskhop-helper.log}"
# The Windows helper's log, live over the share. /Volumes/deskhopplus is the
# helper's own directory on the Windows machine, so this reads what the helper
# is writing right now — no copying, and no stale snapshot to be fooled by.
WIN_LOG="${WIN_LOG:-/Volumes/deskhopplus/helper.log}"
FIXTURES="${FIXTURES:-$HOME/deskhop-clipboard-fixtures}"
MARKS="${MARKS:-$HOME/.deskhop-run-sheet-marks}"

bold=$'\033[1m'; dim=$'\033[2m'; red=$'\033[31m'; green=$'\033[32m'
yellow=$'\033[33m'; blue=$'\033[34m'; off=$'\033[0m'

# ---------------------------------------------------------------- fixtures

# Sizes chosen against the constants, not round numbers: each one sits on the
# far side of a boundary the code actually branches on.
make_files() {
    mkdir -p "$FIXTURES"
    # Under the 1 MB prompt threshold: must cross with no question at all.
    mk 200K quiet-200K.bin
    # Under the 1 MB prompt threshold as well — still no question.
    mk 400K quiet-400K.bin
    # Over a 2 MB cap, under the 10 MB default. The pair for the cap test.
    mk 2600K over-2mb-cap.bin
    # Comfortably under the default cap, long enough to watch progress move.
    mk 5M progress-5M.bin
    # Over the 10 MB default: refused outright while the cap is 10.
    mk 11M over-10mb-cap.bin
    # #39's timed run. Exactly 10485760 bytes, which sits *on* the default cap
    # rather than over it — the board refuses `total > cap`, so this is the
    # largest set that still crosses, and the largest one #39 asks to time.
    mk 10M throughput-10M.bin

    # Several files at once, including an empty one — the offsets are what an
    # empty file breaks, and nothing else in the sheet reaches that.
    mkdir -p "$FIXTURES/set-of-three"
    printf 'first' > "$FIXTURES/set-of-three/one.txt"
    : > "$FIXTURES/set-of-three/two-is-empty.bin"
    printf 'third file here' > "$FIXTURES/set-of-three/three.txt"

    # Twenty files, which is where the metadata limit bites: the list must fit
    # one offer, and past about nineteen ordinary names it will not.
    mkdir -p "$FIXTURES/twenty-files"
    for i in $(seq -w 1 20); do
        printf 'file %s' "$i" > "$FIXTURES/twenty-files/a-fairly-long-file-name-$i.txt"
    done

    printf '%s\n' "${green}fixtures in $FIXTURES${off}"
    ls -la "$FIXTURES" | sed 's/^/  /'
}

mk() {
    local size="$1" name="$2"
    [ -f "$FIXTURES/$name" ] || mkfile -n "$size" "$FIXTURES/$name" 2>/dev/null ||
        dd if=/dev/urandom "of=$FIXTURES/$name" bs=1024 \
           count="$(numfmt --from=iec "${size}" 2>/dev/null | awk '{print int($1/1024)}')" \
           status=none
}

# ------------------------------------------------------------------- sheet

sheet() {
    cat <<SHEET
${bold}The clipboard run sheet — #56${off}

Fixtures: $FIXTURES   (run '$0 files' first)
Run '$0 mark' first, so the counts cover this sitting only.
Start '$0 watch' in another window. It will say what each step did.

${bold}Before anything${off}
  0. Both helpers deployed and running. The watch window should show both
     reaching ${green}Connected and paired${off}. If the Mac shows repeated
     assertion lines and never "started", it is not running at all.

${bold}Mac to Windows${off}
  1. quiet-200K.bin        expect: arrives, ${bold}no question${off}
  2. quiet-400K.bin        expect: arrives, ${bold}still no question${off}
                           (the line is 1 MB — a set under it never asks)
  3. set-of-three/         copy all three files together
                           expect: one question, all three arrive, the empty
                           one is empty and the third is not truncated
  4. progress-5M.bin       expect: one question, progress moves, arrives
  5. progress-5M.bin       accept, then ${bold}cancel${off} partway
                           expect: nothing arrives, nothing partial on disk
  6. over-10mb-cap.bin     expect: ${bold}refused, no question at all${off}
  7. a screenshot          (CleanShot, Cmd-Shift-4 — anything)
                           expect: pastes into ${bold}Paint${off} as a picture,
                           not into Explorer as a .png file
  8. twenty-files/         copy all twenty
                           expect: refused on the copy side with a line about
                           the names not fitting one offer — ${bold}not${off} silence

${bold}Windows to Mac${off} — the same seven, the other way. The Mac's question is
  a panel near the menu bar with Accept and Decline on it.

${bold}The size cap${off}
  9. Set the cap to 2 MB on the config page (field ${bold}Clipboard size cap (MB)${off}),
     on ${bold}both boards${off}.
 10. over-2mb-cap.bin      expect: ${bold}refused, no question${off}
 11. quiet-400K.bin        expect: still works
 12. Put the cap back to 10.

${bold}Lifecycle${off}
 13. Copy files, accept, and while they are arriving pull the USB cable.
     expect: the transfer is abandoned, nothing partial is pasteable
 14. Copy a set on the Mac and ${bold}ignore${off} the question for two minutes.
     expect: it is declined for you, and the link goes quiet
 15. With files on the clipboard, restart a helper, then paste.
     expect: the files are still there

${bold}Throughput — #39${off}
 16. Run '$0 mark' now, so the timings cover this sitting only.
 17. throughput-10M.bin, Mac to Windows. ${bold}Move the mouse the whole time${off}
     and type into something.
     expect: it arrives; the cursor does not stutter
 18. throughput-10M.bin, Windows to Mac. Same again, mouse moving.
     expect: it arrives; the cursor does not stutter
 19. '$0 throughput'
     This prints the per-set rate for each direction, the sustained rate over
     sets above 1 MB, and what that extrapolates to at 64 MB.
     Two columns answer whether bulk hurt anything else while it ran:
     'worst beat gap' is the heartbeat starving (#144), and 'priority refused'
     is how many priority frames the board turned away across the set. A zero
     or a dash in both is the good answer.
SHEET
}

# ------------------------------------------------------------------- watch

# One log line, decoded. Returns nothing for lines that are not worth a person
# reading — which is most of them, and is the point.
interpret() {
    local side="$1" line="$2" tag colour
    case "$side" in
        mac) tag="mac    " ; colour="$blue" ;;
        *)   tag="windows" ; colour="$yellow" ;;
    esac

    local out="" level="info"
    case "$line" in
        *"deskhop helper started"*)          out="helper started" ;;
        *"Connected and paired"*)            out="connected and paired"; level="good" ;;
        *"Assertion failed"*)                out="THE HELPER CRASHED — it is not running"; level="bad" ;;
        *"offered from the other computer"*) out="offer arrived, waiting for an answer here" ;;
        *"were accepted here and asked for"*) out="accepted here" ;;
        *"were declined here"*)              out="declined here" ;;
        *"went unanswered for"*)             out="the question expired and was declined" ;;
        *"arrived whole"*)                   out="files arrived whole"; level="good" ;;
        *"written and put on the"*)          out="files written and put on the clipboard"; level="good" ;;
        *"were offered without being read"*) out="offered — nothing read yet, which is the point" ;;
        *"waiting for a seal before"*)       out="copied here, held until the link is ready" ;;
        *"offering a seal so this end"*)     out="asking the far helper for a shared key" ;;
        *"the seal is live"*)                out="shared key agreed; this end can send"; level="good" ;;
        *"seal accept could not be used"*)   out="THE KEY EXCHANGE FAILED — nothing can be sent"; level="bad" ;;
        *"under the .* line, so they were"*) out="${line#*: }"; level="good" ;;
        *"still waiting for one that can"*)  out="the link dropped; the copy is still held" ;;
        *"the copied files were read"*)      out="files read on the copy side" ;;
        *"could not be read"*)               out="the copied files could not be read"; level="bad" ;;
        *"abandoned rather than sent short"*) out="length changed since the copy — refused, not truncated"; level="good" ;;
        *"so it was refused"*)               out="offer refused (size cap, or a list that does not add up)"; level="good" ;;
        *"so nothing was asked and nothing"*) out="${line#*: }"; level="good" ;;
        *"bytes copied here were offered"*)  out="text or an image was offered from here" ;;
        *"would not fit one offer"*)         out="too many names for one offer — refused on the copy side"; level="good" ;;
        *"did not match the"*)               out="A PAYLOAD DID NOT MATCH ITS LIST — nothing written"; level="bad" ;;
        *"no longer waiting to be asked for"*) out="accepted too late; the transfer had already gone"; level="bad" ;;
        *"size cap is now"*)                 out="${line#*] }" ;;
        *"clipboard policy"*)                out="${line#*: }" ;;
        *"liveness timeout"*)                out="EVICTED — the board saw no sign of life (#161)"; level="bad" ;;
        *"stream had a gap in it"*)          out="EVICTED — the board dropped a report (#161)"; level="bad" ;;
        *"inbound lost"*)                    out="${line#*: }" ;;
        *"before that the board's USB took"*) out="${line#*: }"; level="bad" ;;
        *"Reconnecting repeatedly"*)         out="reconnecting repeatedly (#161)"; level="bad" ;;
        *"made no progress for 30s"*)        out="a transfer was abandoned after 30s of silence"; level="bad" ;;
        *"made no progress for 2s"*)         out="a receive stalled and asked again" ;;
        *"nothing partial is kept"*)         out="cancelled here; nothing partial kept"; level="good" ;;
        *"a folder is not carried"*)         out="a folder was left out of the copy" ;;
        *) return ;;
    esac

    local mark="  "
    [ "$level" = good ] && mark="${green}ok${off}"
    [ "$level" = bad ]  && mark="${red}!!${off}"
    printf '%s %s%s%s  %s\n' "$mark" "$colour" "$tag" "$off" "$out"
}

watch_logs() {
    [ -f "$MAC_LOG" ] || printf '%s\n' "${yellow}no Mac log at $MAC_LOG${off}"
    [ -f "$WIN_LOG" ] || printf '%s\n' "${yellow}no Windows log at $WIN_LOG — is the share mounted?${off}"

    printf '%s\n' "${bold}watching both helpers — ctrl-C to stop${off}"
    printf '%s\n' "${dim}mac: $MAC_LOG${off}"
    printf '%s\n' "${dim}win: $WIN_LOG${off}"
    echo

    # awk, not sed: BSD sed puts a literal "t" where a GNU one puts a tab, so
    # every line arrived unsplittable and the whole window printed nothing.
    { [ -f "$MAC_LOG" ] && tail -n 0 -F "$MAC_LOG" 2>/dev/null | awk '{print "mac\t" $0}' & }
    { [ -f "$WIN_LOG" ] && tail -n 0 -F "$WIN_LOG" 2>/dev/null | awk '{print "win\t" $0}' & }
    wait
}

# `watch` pipes through this so both tails interleave in arrival order.
decode_stream() {
    while IFS=$'\t' read -r side line; do
        # The banner has no tab, so it lands whole in `side`. Passed through
        # rather than dropped, which is how it used to vanish.
        if [ -z "$line" ]; then printf '%s\n' "$side"; continue; fi
        interpret "$side" "$line"
    done
}

# ------------------------------------------------------------------ report

# Both logs are appended to forever, so a plain count reports every crash and
# eviction the machine has ever had. A mark records where each log stands now,
# and `report` counts only past it — which is what a sitting actually wants to
# know.
mark() {
    : > "$MARKS"
    local path n
    for path in "$MAC_LOG" "$WIN_LOG"; do
        n=0
        [ -f "$path" ] && n="$(wc -c < "$path" | tr -d " ")"
        printf '%s\t%s\n' "$path" "$n" >> "$MARKS"
    done
    printf '%s\n' "${green}marked — report now counts only what happens from here${off}"
}

# Bytes to skip in one log. Zero when there is no mark, and zero again when the
# log is shorter than its mark, which means it was rotated or replaced.
mark_offset() {
    local path="$1" want size
    want="$(awk -F'\t' -v p="$path" '$1 == p { print $2 }' "$MARKS" 2>/dev/null)"
    [ -n "${want:-}" ] || { echo 0; return; }
    size="$(wc -c < "$path" 2>/dev/null | tr -d " ")"
    [ "${size:-0}" -lt "$want" ] && { echo 0; return; }
    echo "$want"
}

report() {
    printf '%s\n' "${bold}What the logs say about the last run${off}"
    [ -f "$MARKS" ] || printf '%s\n' \
        "${yellow}no mark set — these are whole-log totals, including old runs.${off}"
    for pair in "mac:$MAC_LOG" "windows:$WIN_LOG"; do
        local name="${pair%%:*}" path="${pair#*:}"
        echo
        printf '%s\n' "${bold}$name${off}  $path"
        [ -f "$path" ] || { printf '  %s\n' "${yellow}not readable${off}"; continue; }
        OFF="$(mark_offset "$path")"
        count "$path" "offers put to the user"        "offered from the other computer"
        count "$path" "accepted"                      "were accepted here and asked for"
        count "$path" "declined"                      "were declined here"
        count "$path" "expired unanswered"            "went unanswered for"
        count "$path" "sets that arrived whole"       "arrived whole"
        count "$path" "offers refused"                "so it was refused"
        count "$path" "copies held for the link"      "waiting for a seal before"
        count "$path" "payload/list mismatches"       "did not match the"       bad
        count "$path" "accepted too late"             "no longer waiting to be asked for" bad
        count "$path" "evicted, no sign of life (#161)" "liveness timeout"      bad
        count "$path" "evicted, lost report (#161)"    "stream had a gap in it"  bad
        count "$path" "helper crashes"                "Assertion failed"        bad
    done
    echo
    printf '%s\n' "${dim}A non-zero count on a line marked !! is a known defect, not your mistake.${off}"
}

count() {
    local path="$1" label="$2" needle="$3" level="${4:-}"
    # grep -c prints the count and still exits 1 when it is zero, so the exit
    # status is discarded and the number it printed is kept.
    local n; n="$(tail -c "+$(( ${OFF:-0} + 1 ))" "$path" 2>/dev/null |
                  grep -cF -- "$needle")" || true
    n="${n:-0}"
    local mark="  "
    [ "$level" = bad ] && [ "$n" -gt 0 ] && mark="${red}!!${off}"
    printf '  %s %-32s %s\n' "$mark" "$label" "$n"
}

# -------------------------------------------------------------- throughput

# The timing reader, as an awk program both log formats go through. The Mac
# helper stamps a wall clock and the Windows one stamps milliseconds of
# uptime, so `fmt` says which to read; everything after that is common.
#
# A set is timed from the line where the receiving helper asked for it
# ("were accepted here and asked for", or "taken without asking" when it was
# under the no-question line) to the line where it "arrived whole". Byte count
# is the join key, because it is the only thing both lines carry.
#
# Newest accept first, deliberately. A set that was accepted and never arrived
# — an eviction (#161) — otherwise stays at the front of the queue and gets
# paired with the *next* run of the same size, which reads as a transfer that
# took three quarters of an hour. Pairing the newest is right because only one
# set is ever in flight; the accepts left over at the end are counted and
# reported as exactly what they are.
timings_program() {
    cat <<'AWK'
function clock(line,   t, h, m, s) {
    if (fmt == "win") {
        if (match(line, /^\[[0-9]+ms\]/))
            return substr(line, 2, RLENGTH - 4) / 1000
        return -1
    }
    if (match(line, /[0-9][0-9]:[0-9][0-9]:[0-9][0-9]\.[0-9][0-9][0-9]/)) {
        t = substr(line, RSTART, RLENGTH)
        h = substr(t, 1, 2); m = substr(t, 4, 2); s = substr(t, 7)
        return h * 3600 + m * 60 + s
    }
    return -1
}
{
    t = clock($0)
    if (t < 0) next

    # Kept so a set's row can say whether the beat went quiet while it ran.
    # Both notes are read: "quiet for" fires while the silence is still going
    # and "resumed after" when it ends, so a gap that outlives the transfer is
    # only ever named by the second one. Each note is turned back into the
    # window it describes — the span ending at the note — and the row takes the
    # longest window that overlaps it, so seeing the same silence twice is
    # harmless.
    if (match($0, /quiet for [0-9.]+s/))      g = substr($0, RSTART + 10, RLENGTH - 11) + 0
    else if (match($0, /resumed after [0-9.]+s/)) g = substr($0, RSTART + 14, RLENGTH - 15) + 0
    else g = 0
    if (g > 0) {
        nq++
        qs[nq] = g
        qend[nq] = t
        qstart[nq] = t - g
    }

    # The board's own count of priority frames it refused because that band was
    # already full — #39's item 3 asked for exactly this read. It is a total
    # since the board booted, so a set's cost is the rise across it. The board
    # states its totals only when something makes it worth saying, so a set
    # with no sample either side reads "-" rather than a made-up zero, and one
    # with samples reads a floor.
    #
    # No `next` here, nor after the beat gap above. The board's totals are not
    # a line of their own — they are appended in brackets to whatever note made
    # them worth stating. Skipping the rest of the line would throw away the
    # note they are riding on, which is how a timed set would go missing.
    if (match($0, /priority [0-9]+,/)) {
        np++
        pt[np] = t
        pv[np] = substr($0, RSTART + 9, RLENGTH - 10) + 0
    }

    # Bytes before files: match() overwrites RSTART and RLENGTH.
    if (!match($0, /[0-9]+ bytes/)) next
    b = substr($0, RSTART, RLENGTH - 6) + 0
    f = 0
    if (match($0, /[0-9]+ file\(s\)/)) f = substr($0, RSTART, RLENGTH - 8) + 0

    if (index($0, "were accepted here and asked for") ||
        index($0, "taken without asking")) {
        i = tail[b]++
        st[b, i] = t
        sf[b, i] = f
        next
    }

    if (index($0, "arrived whole")) {
        i = tail[b] - 1
        if (i < 0) { orphan++; next }
        tail[b] = i
        d = t - st[b, i]
        # The Mac stamp is a time of day, so a run over midnight reads
        # negative. Uptime never wraps, so a negative one there is a restart.
        if (d < 0 && fmt != "win") d += 86400
        if (d <= 0 || d > 21600) { skipped++; next }
        rows++
        rb[rows] = b; rf[rows] = sf[b, i]; rd[rows] = d
        r0[rows] = st[b, i]; r1[rows] = t
        if (b >= 1048576) { sumb += b; sumd += d }
    }
}
END {
    if (rows == 0)
        print "  no completed transfers to time"
    else {
        printf "  %10s %6s %8s %7s  %-14s %s\n", "bytes", "files", "seconds", "KB/s",
               "worst beat gap", "priority refused"
        for (r = 1; r <= rows; r++) {
            worst = 0
            for (k = 1; k <= nq; k++)
                if (qend[k] >= r0[r] && qstart[k] <= r1[r] && qs[k] > worst) worst = qs[k]
            gap = "-"
            if (worst > 0) gap = sprintf("%.1fs", worst)
            base = -1; final = -1
            for (k = 1; k <= np; k++) {
                if (pt[k] <= r0[r]) base = pv[k]
                if (pt[k] <= r1[r]) final = pv[k]
            }
            pri = "-"
            if (base >= 0 && final >= base) pri = final - base
            else if (base >= 0)             pri = "reset"
            printf "  %10d %6d %8.1f %7.1f  %-14s %s\n", rb[r], rf[r], rd[r],
                   rb[r] / rd[r] / 1024, gap, pri
        }
    }
    # Sets under 1 MB are dominated by the round trip that starts them, so a
    # sustained figure taken over those flatters the link. Over 1 MB only.
    if (sumd > 0) {
        printf "\n  sustained (sets over 1 MB): %d bytes in %.1f s = %.1f KB/s\n",
               sumb, sumd, sumb / sumd / 1024
        printf "  at that rate: 10 MB in %.0f s, 64 MB in %.0f min\n",
               10485760 / (sumb / sumd), 67108864 / (sumb / sumd) / 60
    }
    unfinished = 0
    for (key in tail) unfinished += tail[key]
    # Cancelling one is a step on the sheet, so this is not a failure count.
    if (unfinished)
        printf "  %d accepted set(s) with no arrival — cancelled, abandoned, or lost\n", unfinished
    if (orphan)  printf "  %d arrival(s) had no accept in range — run 'mark' before the sitting\n", orphan
    if (skipped) printf "  %d pair(s) skipped: the helper restarted mid-transfer\n", skipped
}
AWK
}

throughput() {
    printf '%s\n' "${bold}How fast each set actually moved${off}"
    [ -f "$MARKS" ] || printf '%s\n' \
        "${yellow}no mark set — these are whole-log timings, including old runs.${off}"
    local prog
    prog="$(timings_program)"
    timings_for mac "$MAC_LOG" mac "$prog"
    timings_for windows "$WIN_LOG" win "$prog"
    echo
    printf '%s\n' "${dim}Each side times what arrived *at* it, so the two tables are the two directions.${off}"
}

# One side's table. `fmt` is which stamp its log carries, and the mark is
# honoured here for the same reason `report` honours it: a sitting wants its
# own numbers, not every run the machine has ever done.
timings_for() {
    local name="$1" path="$2" fmt="$3" prog="$4"
    echo
    printf '%s\n' "${bold}arriving at $name${off}  $path"
    [ -f "$path" ] || { printf '  %s\n' "${yellow}not readable${off}"; return; }
    tail -c "+$(( $(mark_offset "$path") + 1 ))" "$path" | awk -v fmt="$fmt" "$prog"
}

# ---------------------------------------------------------------- selftest

# One check, on the only part of this script with logic in it. The fixture
# covers both stamp formats, an accept that never arrived sitting in front of
# a later run of the same size, an arrival with no accept, and a beat gap
# landing inside a set's window.
selftest() {
    local prog out want fails=0
    prog="$(timings_program)"

    out="$(printf '%s\n' \
        '2026-09-03 21:00:00.000  +1.0s  deskhop-helper: 1 file(s), 204800 bytes, were accepted here and asked for' \
        '2026-09-03 21:00:04.000  +5.0s  deskhop-helper: 1 file(s), 204800 bytes, arrived whole' \
        '2026-09-03 21:00:09.000  +10.0s deskhop-helper: board drops: outbound refused 4 (priority 4, bulk 0, bad header 0); board inbound: 9 report(s) in' \
        '2026-09-03 21:00:10.000  +11.0s deskhop-helper: 1 file(s), 2097152 bytes, were accepted here and asked for' \
        '2026-09-03 21:00:30.000  +31.0s deskhop-helper: device heartbeat quiet for 5.0s' \
        '2026-09-03 21:01:00.000  +61.0s deskhop-helper: device heartbeat resumed after 17.3s' \
        '2026-09-03 21:01:14.000  +75.0s deskhop-helper: 1 file(s), 2097152 bytes, arrived whole (board drops: outbound refused 9 (priority 9, bulk 0, bad header 0))' \
        '2026-09-03 21:02:00.000  +121.0s deskhop-helper: 1 file(s), 4096 bytes, arrived whole' \
        | awk -v fmt=mac "$prog")"
    want='      204800      1      4.0    50.0  -              -
     2097152      1     64.0    32.0  17.3s          5'
    check_has "mac rows" "$out" "$want" || fails=1
    check_has "mac sustained" "$out" 'sustained (sets over 1 MB): 2097152 bytes in 64.0 s = 32.0 KB/s' || fails=1
    check_has "mac orphan" "$out" '1 arrival(s) had no accept in range' || fails=1

    # The eviction case: 500000 accepted and lost, then accepted again and
    # arrived. Pairing the oldest would call that second run 100 seconds.
    out="$(printf '%s\n' \
        '[10000ms] 1 file(s), 500000 bytes, were accepted here and asked for' \
        '[90000ms] board drops: outbound refused 7 (priority 7, bulk 0, bad header 0); board inbound: 9 report(s) in' \
        '[100000ms] 1 file(s), 500000 bytes, are under the 1 MB line, so they were taken without asking' \
        '[105000ms] board drops: outbound refused 2 (priority 2, bulk 0, bad header 0); board inbound: 9 report(s) in' \
        '[110000ms] 1 file(s), 500000 bytes, arrived whole' \
        | awk -v fmt=win "$prog")"
    check_has "win newest-first pairing" "$out" '      500000      1     10.0    48.8  -              reset' || fails=1
    check_has "win unfinished" "$out" '1 accepted set(s) with no arrival' || fails=1

    # Midnight: the Mac stamp wraps, and the set still took ten seconds.
    out="$(printf '%s\n' \
        '2026-09-03 23:59:55.000  +1.0s  deskhop-helper: 1 file(s), 204800 bytes, were accepted here and asked for' \
        '2026-09-04 00:00:05.000  +11.0s deskhop-helper: 1 file(s), 204800 bytes, arrived whole' \
        | awk -v fmt=mac "$prog")"
    check_has "mac midnight" "$out" '      204800      1     10.0    20.0  -' || fails=1

    if [ "$fails" = 0 ]; then
        printf '%s\n' "${green}selftest passed${off}"
    else
        printf '%s\n' "${red}selftest FAILED${off}"
    fi
    return "$fails"
}

check_has() {
    local label="$1" got="$2" want="$3"
    case "$got" in
        *"$want"*) printf '  %s %s\n' "${green}ok${off}" "$label"; return 0 ;;
    esac
    printf '  %s %s\n' "${red}!!${off}" "$label"
    printf '     wanted: %s\n' "$want"
    printf '     got:\n'
    printf '%s\n' "$got" | sed 's/^/       /'
    return 1
}

case "${1:-sheet}" in
    files)  make_files ;;
    mark)   mark ;;
    sheet)  sheet ;;
    watch)  watch_logs | decode_stream ;;
    report) report ;;
    throughput) throughput ;;
    selftest)   selftest ;;
    *)      printf 'usage: %s {files|mark|sheet|watch|report|throughput|selftest}\n' "$0"; exit 2 ;;
esac
