#!/usr/bin/env bash
# Headless smoke test: pipe commands in, assert ok:/err: lines and structured events appear.
set -euo pipefail

BINARY=".pio/build/native_cli/program"
[ -x "$BINARY" ] || { echo "build the binary first: pio run -e native_cli" >&2; exit 1; }

OUT=$(mktemp)
trap 'rm -f "$OUT"' EXIT

# Scenario 1: basic REPL responses + ANSI hygiene.
printf 'state\nbogus_command_xyz\nquit\n' | "$BINARY" --headless 2 >"$OUT" || true

grep -q "^ready devices=2 pid=" "$OUT" || { echo "missing ready line"; cat "$OUT"; exit 1; }
grep -qE "^ok:" "$OUT"             || { echo "missing ok: response"; cat "$OUT"; exit 1; }
grep -qE "^err:" "$OUT"            || { echo "missing err: response"; cat "$OUT"; exit 1; }
if grep -q $'\033' "$OUT"; then
    echo "stdout contains ANSI escape sequences"; cat "$OUT"; exit 1
fi

# Scenario 2: event lines for state transitions, cable connection, and button press.
{
    printf 'cable 0 1\n'
    sleep 0.5
    printf 'press 0 primary\n'
    sleep 0.2
    printf 'quit\n'
} | "$BINARY" --headless 2 >"$OUT" || true

grep -qE "^event ts=[0-9]+ device=0 kind=state_transition" "$OUT" \
    || { echo "missing state_transition event"; cat "$OUT"; exit 1; }
grep -qE "^event ts=[0-9]+ device=0 kind=cable_connected peer=1" "$OUT" \
    || { echo "missing cable_connected event"; cat "$OUT"; exit 1; }
grep -qE "^event ts=[0-9]+ device=0 kind=button_press button=primary" "$OUT" \
    || { echo "missing button_press event"; cat "$OUT"; exit 1; }

# Scenario 3: tick command and events on|off toggle.
OUT3=$(mktemp)
{
    printf 'events off\n'
    printf 'cable 0 1\n'
    sleep 0.5
    printf 'events on\n'
    printf 'tick 200\n'
    printf 'quit\n'
} | "$BINARY" --headless 2 >"$OUT3" || true

grep -qE "^ok: tick 200" "$OUT3" \
    || { echo "missing tick ok response"; cat "$OUT3"; rm -f "$OUT3"; exit 1; }
# events between events-off and events-on must be absent
if awk '/^ok: events off/{flag=1; next} /^ok: events on/{flag=0; next} flag && /^event /' "$OUT3" | grep -q .; then
    echo "events emitted while events were off"; cat "$OUT3"; rm -f "$OUT3"; exit 1
fi
rm -f "$OUT3"

echo "headless_smoke OK"
