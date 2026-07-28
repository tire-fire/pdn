#pragma once

#include "game/quickdraw-states.hpp"
#include "wireless/remote-debug-manager.hpp"
#include "state/state.hpp"
#include <functional>
#include <vector>

/// Entry points one sub-graph exposes to the others, plus the one abort
/// predicate every state a live tournament can interrupt shares.
///
/// A transition list is a priority list — `State::checkTransitions` returns the
/// first condition that holds — so a state's edges have to be added in one
/// place, in order. That is why the cross-graph edges are handed to the graph
/// that owns the *source* state instead of being appended afterwards by the
/// orchestrator: appending would silently demote every cross-graph edge below
/// the intra-graph ones.
struct CrossGraphTargets {
    ShootoutProposal* shootoutProposal = nullptr;
    ShootoutSpectator* shootoutSpectator = nullptr;
    ShootoutEliminated* shootoutEliminated = nullptr;
    ShootoutAborted* shootoutAborted = nullptr;
    SymbolState* symbol = nullptr;
    Idle* idle = nullptr;
    DuelCountdown* duelCountdown = nullptr;
    Sleep* sleep = nullptr;
    /// Held once and copied into each of the six states that route to
    /// ShootoutAborted; re-spelling it per state would fork the abort rule.
    std::function<bool()> phaseIsAborted;
};

/// Sleep -> Awaken -> Idle -> DuelCountdown -> Duel ->
/// DuelPushed / DuelReceivedResult -> DuelResult -> Win|Lose -> UploadMatches -> Sleep.
///
/// The pointers are non-owning handles onto states this graph allocates;
/// `appendTo` hands ownership to the state map, whose owner deletes them.
struct DuelGraph {
    AwakenSequence* awakenSequence = nullptr;
    Idle* idle = nullptr;
    SupporterReady* supporterReady = nullptr;
    DuelCountdown* duelCountdown = nullptr;
    Duel* duel = nullptr;
    DuelPushed* duelPushed = nullptr;
    DuelReceivedResult* duelReceivedResult = nullptr;
    DuelResult* duelResult = nullptr;
    Win* win = nullptr;
    Lose* lose = nullptr;
    UploadMatchesState* uploadMatches = nullptr;
    Sleep* sleep = nullptr;

    /// Allocates the states. Split from wiring because the graphs reference
    /// each other's entry points, so every state must exist before any edge is
    /// added.
    void build(const GameContext& ctx);
    /// Adds every transition leaving a duel-graph state, in priority order.
    void wire(const GameContext& ctx, const CrossGraphTargets& cross) const;
    /// Appends the states in registration order and gives up ownership.
    void appendTo(std::vector<State*>& stateMap) const;
};

/// ShootoutProposal -> BracketReveal -> Spectator | Eliminated -> FinalStandings,
/// plus the Aborted landing state. Nothing here adds an abort *edge* onto a
/// duel-graph state: ShootoutAwareState::tickAbortGuard raises the abort and
/// `CrossGraphTargets::phaseIsAborted` is the single predicate that observes it.
struct ShootoutGraph {
    ShootoutProposal* proposal = nullptr;
    ShootoutBracketReveal* bracketReveal = nullptr;
    ShootoutSpectator* spectator = nullptr;
    ShootoutEliminated* eliminated = nullptr;
    ShootoutFinalStandings* finalStandings = nullptr;
    ShootoutAborted* aborted = nullptr;

    /// Allocates the states; see DuelGraph::build for why wiring is separate.
    void build(const GameContext& ctx);
    /// Adds every transition leaving a shootout state, in priority order.
    void wire(const CrossGraphTargets& cross) const;
    /// Appends the states in registration order and gives up ownership.
    void appendTo(std::vector<State*>& stateMap) const;
};

/// Symbol -> SymbolMatched -> Symbol | Idle.
struct SymbolGraph {
    SymbolState* symbol = nullptr;
    SymbolMatched* symbolMatched = nullptr;

    /// Allocates the states; see DuelGraph::build for why wiring is separate.
    void build(const GameContext& ctx);
    /// Adds every transition leaving a symbol-match state, in priority order.
    void wire(const CrossGraphTargets& cross) const;
    /// Appends the states in registration order and gives up ownership.
    void appendTo(std::vector<State*>& stateMap) const;
};

namespace QuickdrawStateBuilder {

/// Builds the three sub-graphs, wires the edges that span them, and appends
/// every state to `stateMap` in registration order — index 0 is the initial
/// state. `stateMap`'s owner deletes all of them.
///
/// Returns the SupporterReady state, which Quickdraw dispatches inbound chain
/// game events to while it is the mounted state.
SupporterReady* build(std::vector<State*>& stateMap, const GameContext& ctx, RemoteDebugManager* remoteDebugManager);

}  // namespace QuickdrawStateBuilder
