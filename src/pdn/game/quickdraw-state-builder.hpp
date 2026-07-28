#pragma once

#include "game/quickdraw-states.hpp"
#include "wireless/remote-debug-manager.hpp"
#include "state/state.hpp"
#include <vector>

namespace QuickdrawStateBuilder {

/// Builds the three sub-graphs, wires the edges that span them, and appends
/// every state to `stateMap` in registration order — index 0 is the initial
/// state. `stateMap`'s owner deletes all of them.
///
/// Returns the SupporterReady state, which Quickdraw dispatches inbound chain
/// game events to while it is the mounted state.
SupporterReady* build(std::vector<State*>& stateMap, const GameContext& ctx, RemoteDebugManager* remoteDebugManager);

}  // namespace QuickdrawStateBuilder
