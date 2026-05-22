#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

scripts/sim.py start 2
trap 'scripts/sim.py stop || true' EXIT

# Toggle cable rapidly.
for _ in 1 2 3 4; do
    scripts/sim.py cable 0 1
    scripts/sim.py cmd -- cable -d 0 1
done

# Final connect; assert both devices got cable_connected with peer=other.
scripts/sim.py cable 0 1
scripts/sim.py wait-event "device=0 kind=cable_connected peer=1" --timeout 3
scripts/sim.py wait-event "device=1 kind=cable_connected peer=0" --timeout 3

# Assert no error events were emitted during the toggling.
errs=$(scripts/sim.py events --match "kind=error" || true)
if [ -n "$errs" ]; then
    echo "error events emitted during rapid toggle"
    echo "$errs"
    exit 1
fi
echo "rapid_cable_toggle OK"
