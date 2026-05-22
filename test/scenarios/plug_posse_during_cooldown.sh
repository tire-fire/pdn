#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# A Hunter+Bounty pair is required to reach Duel.  Start with one Hunter,
# add a Bounty, cable them, drive through the duel, then plug in a third device
# during the post-duel Sleep phase.  Note: PosseReady is the supporter state
# in a Hunter-Hunter chain, not the post-duel state; after a real duel, devices
# go Win/Lose -> UploadMatches -> Sleep.
scripts/sim.py start 1
trap 'scripts/sim.py stop || true' EXIT

scripts/sim.py add bounty

# Two-device duel: hunter (0) and bounty (1).
scripts/sim.py cable 0 1

# Drive into Duel, then press to end the round.
scripts/sim.py wait-event --regex "to=Duel$" --timeout 30
scripts/sim.py press 0 primary
scripts/sim.py press 1 primary

# Wait for the post-duel Sleep phase (Win/Lose -> UploadMatches -> Sleep).
scripts/sim.py wait-event "to=Sleep" --timeout 20

# Plug in device 2 as posse during Sleep.
scripts/sim.py add
scripts/sim.py cable 0 2
scripts/sim.py wait-event "device=2 kind=cable_connected peer=0" --timeout 5

# Expect handshake follow-up -- at least one state_transition on device 2.
scripts/sim.py wait-event "device=2 kind=state_transition" --timeout 10

echo "plug_posse_during_cooldown OK"
