#!/usr/bin/env bash
# Single instance over D-Bus: a second invocation forwards its files (relative
# paths resolved in its own directory) to the running instance and exits;
# -multiInst starts a separate process that runs without a session.
#
# Usage: single_instance.sh <notepad-plus-plus-gtk>. Needs dbus-run-session
# (a private session bus); exits 77 (skipped) without it.
set -euo pipefail

exe=$1
command -v dbus-run-session >/dev/null || { echo "dbus-run-session not found: skipping"; exit 77; }

if [[ ${NPP_IN_BUS:-} != 1 ]]; then
    # Services the bus activates (portals) may outlive the test and would keep
    # ctest's output pipe open: give them a log file instead.
    log=$(mktemp)
    status=0
    env NPP_IN_BUS=1 dbus-run-session -- "$0" "$@" >"$log" 2>&1 </dev/null || status=$?
    grep -v -e '^dbus-daemon\[' -e 'portal' -e 'kwallet' -e 'ksecretd' -e 'qt.qpa' -e '^$' "$log" || true
    rm -f "$log"
    exit "$status"
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export XDG_CONFIG_HOME=$work/config
mkdir -p "$work/files"
printf 'first\n' > "$work/files/first.txt"
printf 'second\n' > "$work/files/second.txt"
printf 'third\n' > "$work/files/third.txt"

"$exe" --exit-after=4000 "$work/files/first.txt" &
primary=$!
sleep 1.5

# Forwarded: returns at once with the primary's exit status.
start=$(date +%s%N)
(cd "$work/files" && "$exe" -n2 second.txt)
elapsed_ms=$(( ($(date +%s%N) - start) / 1000000 ))
echo "second invocation returned after ${elapsed_ms} ms"
[[ $elapsed_ms -lt 1500 ]] || { echo "FAIL: second invocation did not hand over"; exit 1; }

# -multiInst: its own process; must not write the shared session.
"$exe" -multiInst --exit-after=300 "$work/files/third.txt"

wait "$primary"
session=$XDG_CONFIG_HOME/notepad-plus-plus-gtk/session.xml
cat "$session"
grep -q "first.txt" "$session" || { echo "FAIL: first.txt missing from session"; exit 1; }
grep -q "$work/files/second.txt" "$session" || { echo "FAIL: forwarded file missing from session"; exit 1; }
if grep -q "third.txt" "$session"; then echo "FAIL: -multiInst instance wrote the session"; exit 1; fi
echo "single instance OK"
