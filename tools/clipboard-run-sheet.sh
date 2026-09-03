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
    # Under the 256 KB prompt threshold: must cross with no question at all.
    mk 200K quiet-200K.bin
    # Over the prompt threshold, under every cap: one question, one transfer.
    mk 400K asks-400K.bin
    # Over a 2 MB cap, under the 10 MB default. The pair for the cap test.
    mk 2600K over-2mb-cap.bin
    # Comfortably under the default cap, long enough to watch progress move.
    mk 5M progress-5M.bin
    # Over the 10 MB default: refused outright while the cap is 10.
    mk 11M over-10mb-cap.bin

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
  2. asks-400K.bin         expect: one question, accept, arrives
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
 11. asks-400K.bin         expect: still works
 12. Put the cap back to 10.

${bold}Lifecycle${off}
 13. Copy files, accept, and while they are arriving pull the USB cable.
     expect: the transfer is abandoned, nothing partial is pasteable
 14. Copy a set on the Mac and ${bold}ignore${off} the question for two minutes.
     expect: it is declined for you, and the link goes quiet
 15. With files on the clipboard, restart a helper, then paste.
     expect: the files are still there
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
        *"still waiting for one that can"*)  out="the link dropped; the copy is still held" ;;
        *"the copied files were read"*)      out="files read on the copy side" ;;
        *"could not be read"*)               out="the copied files could not be read"; level="bad" ;;
        *"abandoned rather than sent short"*) out="length changed since the copy — refused, not truncated"; level="good" ;;
        *"so it was refused"*)               out="offer refused (size cap, or a list that does not add up)"; level="good" ;;
        *"would not fit one offer"*)         out="too many names for one offer — refused on the copy side"; level="good" ;;
        *"did not match the"*)               out="A PAYLOAD DID NOT MATCH ITS LIST — nothing written"; level="bad" ;;
        *"no longer waiting to be asked for"*) out="accepted too late; the transfer had already gone"; level="bad" ;;
        *"size cap is now"*)                 out="${line#*] }" ;;
        *"clipboard policy"*)                out="${line#*: }" ;;
        *"liveness timeout"*)                out="EVICTED — the board saw no sign of life (#161)"; level="bad" ;;
        *"stream had a gap in it"*)          out="EVICTED — the board dropped a report (#161)"; level="bad" ;;
        *"inbound lost"*)                    out="${line#*: }" ;;
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

    { [ -f "$MAC_LOG" ] && tail -n 0 -F "$MAC_LOG" 2>/dev/null | sed 's/^/mac\t/' & }
    { [ -f "$WIN_LOG" ] && tail -n 0 -F "$WIN_LOG" 2>/dev/null | sed 's/^/win\t/' & }
    wait
}

# `watch` pipes through this so both tails interleave in arrival order.
decode_stream() {
    while IFS=$'\t' read -r side line; do
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

case "${1:-sheet}" in
    files)  make_files ;;
    mark)   mark ;;
    sheet)  sheet ;;
    watch)  watch_logs | decode_stream ;;
    report) report ;;
    *)      printf 'usage: %s {files|mark|sheet|watch|report}\n' "$0"; exit 2 ;;
esac
