#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# A Hunter+Bounty pair is required to reach Duel.  The default sim creates all
# Hunters (PRIMARY-AUXILIARY cabling), so start with one Hunter and add a Bounty
# explicitly so SerialCableBroker wires them PRIMARY-to-PRIMARY as the game expects.
scripts/sim.py start 1
trap 'scripts/sim.py stop || true' EXIT

scripts/sim.py add bounty
scripts/sim.py cable 0 1

# Wait until we reach Duel on either device.  Use --regex with a trailing $
# so "to=Duel" does not match "to=DuelCountdown".
scripts/sim.py wait-event --regex "to=Duel$" --timeout 30

# Withhold press: do nothing for a generous window.
sleep 5

# No button_press events should have appeared in the interval.
events_during=$(scripts/sim.py events --match "kind=button_press" || true)
if [ -n "$events_during" ]; then
    echo "unexpected button_press during no-press scenario"
    echo "$events_during"
    exit 1
fi

# Assert a transition out of Duel occurred (any next state qualifies).
trans=$(scripts/sim.py events --match "kind=state_transition from=Duel" | head -1 || true)
if [ -z "$trans" ]; then
    echo "no state transition out of Duel observed"
    exit 1
fi

echo "no_press_during_quickdraw OK"
