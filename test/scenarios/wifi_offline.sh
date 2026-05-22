#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# A Hunter+Bounty pair is required to reach Duel.
scripts/sim.py start 1
trap 'scripts/sim.py stop || true' EXIT

scripts/sim.py add bounty

# Take the mock HTTP server offline before any match attempts upload.
scripts/sim.py http offline

# Drive a quick duel.
scripts/sim.py cable 0 1
scripts/sim.py wait-event --regex "to=Duel$" --timeout 30
scripts/sim.py press 0 primary
scripts/sim.py press 1 primary

# Wait for the match to reach its post-result phase (Sleep, via UploadMatches).
scripts/sim.py wait-event "to=Sleep" --timeout 20

# Assert no crash-like error events fired.
if scripts/sim.py events --match "kind=error" | grep -iE "(segfault|crash|abort)"; then
    echo "device crashed while WiFi offline"
    exit 1
fi

echo "wifi_offline OK"
