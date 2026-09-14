#!/bin/bash
#
# build.sh's helper report must ask launchd whether the agent is up, not the
# filesystem (#192): a job bootstrapped from a plist outside ~/Library/LaunchAgents
# was reported as "not installed" with "one in the foreground", so the kickstart
# reminder from #93 never appeared and the job kept running the old binary.
#
# A fake launchctl stands in for launchd. The plist it names is never created:
# the /tmp copy in #192 was gone by the next day while the job it bootstrapped
# was still running, so launchd has to answer for the binary as well. The
# binary is dated in the future so the running process predates the build,
# which is #93's exact trap.
set -euo pipefail
cd "$(dirname "$0")/../.."

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
plist="$tmp/deskhop-192.plist"
bin="$tmp/.build/release/deskhop-helper"

# The job is loaded, its pid is this shell's, and its plist is nowhere near
# ~/Library/LaunchAgents. Only the two subcommands build.sh uses are answered,
# with the line shapes of the real ones -- including the "stderr path" that a
# loose match for "path" would pick up.
mkdir -p "$tmp/bin"
cat >"$tmp/bin/launchctl" <<EOF
#!/bin/sh
case "\$1" in
    list)  printf '{\n\t"PID" = $$;\n};\n' ;;
    print) printf '\tstderr path = /tmp/deskhop-helper.log\n\tpath = $plist\n\tstate = running\n\tprogram = $bin\n' ;;
    *)     exit 1 ;;
esac
EOF
chmod +x "$tmp/bin/launchctl"

mkdir -p "$(dirname "$bin")"
touch -t "$(date -v+1y +%Y%m%d%H%M)" "$bin"

PATH="$tmp/bin:$PATH"
. tools/build.sh
repo="$tmp"; cd "$repo"
helper_plist_installed="$tmp/absent.plist"

out="$(report_helper)"

fail=0
expect() {
    case "$out" in *"$1"*) ;; *) echo "FAIL: missing '$1'"; fail=1 ;; esac
}
reject() {
    case "$out" in *"$1"*) echo "FAIL: unexpected '$1'"; fail=1 ;; esac
}
expect "$plist"
expect "installed $bin"
expect 'launchctl kickstart -k'
reject 'not installed'
reject 'foreground'
reject 'ProgramArguments'

# The fake report only on failure: this also runs inside build.sh, whose real
# report follows and must not be mistaken for it.
if [ "$fail" = 1 ]; then printf '%s\n' "$out"; exit 1; fi
echo "Build report checks passed"
