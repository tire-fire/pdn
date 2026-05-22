#!/usr/bin/env bash
# Headless smoke test: pipe a few commands in, assert ok:/err: lines appear.
set -euo pipefail

BINARY=".pio/build/native_cli/program"
[ -x "$BINARY" ] || { echo "build the binary first: pio run -e native_cli" >&2; exit 1; }

OUT=$(mktemp)
trap 'rm -f "$OUT"' EXIT

printf 'state\nbogus_command_xyz\nquit\n' | "$BINARY" --headless 2 >"$OUT" || true

grep -q "^ready devices=2 pid=" "$OUT" || { echo "missing ready line"; cat "$OUT"; exit 1; }
grep -qE "^ok:" "$OUT"             || { echo "missing ok: response"; cat "$OUT"; exit 1; }
grep -qE "^err:" "$OUT"            || { echo "missing err: response"; cat "$OUT"; exit 1; }

# Stdout must not contain ANSI escapes in headless mode.
if grep -q $'\033' "$OUT"; then
    echo "stdout contains ANSI escape sequences"; cat "$OUT"; exit 1
fi

echo "headless_smoke OK"
