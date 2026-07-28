#include "game/quickdraw-state-builder.hpp"
#include "apps/player-registration/player-registration.hpp"
#include <functional>

namespace {

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

}  // namespace

void DuelGraph::build(const GameContext& ctx) {
    awakenSequence = new AwakenSequence(ctx);
    idle = new Idle(ctx);
    supporterReady = new SupporterReady(ctx);
    duelCountdown = new DuelCountdown(ctx);
    duel = new Duel(ctx);
    duelPushed = new DuelPushed(ctx);
    duelReceivedResult = new DuelReceivedResult(ctx);
    duelResult = new DuelResult(ctx);
    win = new Win(ctx);
    lose = new Lose(ctx);
    uploadMatches = new UploadMatchesState(ctx);
    sleep = new Sleep(ctx);
}

void DuelGraph::wire(const GameContext& ctx, const CrossGraphTargets& cross) const {
    // The transition lambdas capture these by value. They alias the members
    // deliberately: this graph is a stack temporary in QuickdrawStateBuilder::build,
    // so a lambda capturing `this` would dangle the moment the map is registered.
    AwakenSequence* awakenSequence = this->awakenSequence;
    Idle* idle = this->idle;
    SupporterReady* supporterReady = this->supporterReady;
    DuelCountdown* duelCountdown = this->duelCountdown;
    Duel* duel = this->duel;
    DuelPushed* duelPushed = this->duelPushed;
    DuelReceivedResult* duelReceivedResult = this->duelReceivedResult;
    DuelResult* duelResult = this->duelResult;
    Win* win = this->win;
    Lose* lose = this->lose;
    UploadMatchesState* uploadMatches = this->uploadMatches;
    Sleep* sleep = this->sleep;
    ChainDuelManager* chainDuelManager = ctx.chainDuelManager;
    ShootoutManager* shootoutManager = ctx.shootoutManager;

    awakenSequence->addTransition(
        new StateTransition(
            [awakenSequence]() { return awakenSequence->transitionToIdle(); },
            idle));

    // Auto-trigger Shootout when the chain closes into a loop. Priority over
    // DuelCountdown/SupporterReady: in a closed ring, adjacent H-B pairs
    // otherwise look like a normal duel initiation, and the ring intent
    // (tournament) would be silently demoted to a 1v1 duel.
    idle->addTransition(
        new StateTransition(
            [chainDuelManager, shootoutManager]() {
                bool loop = chainDuelManager && chainDuelManager->isLoop();
                bool shootoutActive = shootoutManager && shootoutManager->active();
                return loop && shootoutManager && !shootoutActive;
            },
            cross.shootoutProposal));

    idle->addTransition(
        new StateTransition(
            [idle]() { return idle->transitionToDuelCountdown(); },
            duelCountdown));

    idle->addTransition(
        new StateTransition(
            [idle]() { return idle->transitionToSupporterReady(); },
            supporterReady));

    idle->addTransition(new StateTransition(cross.phaseIsAborted, cross.shootoutAborted));

    idle->addTransition(
        new StateTransition(
            [idle]() { return idle->transitionToSymbol(); },
            cross.symbol));

    supporterReady->addTransition(
        new StateTransition(
            [supporterReady]() { return supporterReady->transitionToIdle(); },
            idle));

    duelCountdown->addTransition(new StateTransition(cross.phaseIsAborted, cross.shootoutAborted));

    duelCountdown->addTransition(
        new StateTransition(
            [duelCountdown]() { return duelCountdown->shallWeBattle(); },
            duel));

    duelCountdown->addTransition(
        new StateTransition(
            [duelCountdown, shootoutManager]() {
                return duelReturnsToIdle(*duelCountdown, shootoutManager);
            },
            idle));

    duel->addTransition(new StateTransition(cross.phaseIsAborted, cross.shootoutAborted));

    duel->addTransition(
        new StateTransition(
            [duel]() { return duel->transitionToShootoutSpectator(); },
            cross.shootoutSpectator));

    duel->addTransition(
        new StateTransition(
            [duel]() { return duel->transitionToShootoutEliminated(); },
            cross.shootoutEliminated));

    duel->addTransition(
        new StateTransition(
            [duel]() { return duel->transitionToIdle(); },
            idle));

    duel->addTransition(
        new StateTransition(
            [duel]() { return duel->transitionToDuelReceivedResult(); },
            duelReceivedResult));

    duel->addTransition(
        new StateTransition(
            [duel]() { return duel->transitionToDuelPushed(); },
            duelPushed));

    duelPushed->addTransition(new StateTransition(cross.phaseIsAborted, cross.shootoutAborted));

    duelPushed->addTransition(
        new StateTransition(
            [duelPushed, shootoutManager]() {
                return duelReturnsToIdle(*duelPushed, shootoutManager);
            },
            idle));

    duelPushed->addTransition(
        new StateTransition(
            [duelPushed]() { return duelPushed->transitionToDuelResult(); },
            duelResult));

    duelReceivedResult->addTransition(new StateTransition(cross.phaseIsAborted, cross.shootoutAborted));

    duelReceivedResult->addTransition(
        new StateTransition(
            [duelReceivedResult, shootoutManager]() {
                return duelReturnsToIdle(*duelReceivedResult, shootoutManager);
            },
            idle));

    duelReceivedResult->addTransition(
        new StateTransition(
            [duelReceivedResult]() { return duelReceivedResult->transitionToDuelResult(); },
            duelResult));

    duelResult->addTransition(new StateTransition(cross.phaseIsAborted, cross.shootoutAborted));

    duelResult->addTransition(
        new StateTransition(
            [duelResult]() { return duelResult->transitionToWin(); },
            win));

    duelResult->addTransition(
        new StateTransition(
            [duelResult]() { return duelResult->transitionToLose(); },
            lose));

    duelResult->addTransition(
        new StateTransition(
            [duelResult]() { return duelResult->transitionToShootoutSpectator(); },
            cross.shootoutSpectator));

    duelResult->addTransition(
        new StateTransition(
            [duelResult]() { return duelResult->transitionToShootoutEliminated(); },
            cross.shootoutEliminated));

    win->addTransition(
        new StateTransition(
            [win]() { return win->resetGame(); },
            uploadMatches));

    lose->addTransition(
        new StateTransition(
            [lose]() { return lose->resetGame(); },
            uploadMatches));

    uploadMatches->addTransition(
        new StateTransition(
            [uploadMatches]() { return uploadMatches->transitionToSleep(); },
            sleep));

    sleep->addTransition(
        new StateTransition(
            [sleep]() { return sleep->transitionToAwakenSequence(); },
            awakenSequence));
}

void DuelGraph::appendTo(std::vector<State*>& stateMap) const {
    stateMap.push_back(awakenSequence);
    stateMap.push_back(idle);
    stateMap.push_back(supporterReady);
    stateMap.push_back(duelCountdown);
    stateMap.push_back(duel);
    stateMap.push_back(duelPushed);
    stateMap.push_back(duelReceivedResult);
    stateMap.push_back(duelResult);
    stateMap.push_back(win);
    stateMap.push_back(lose);
    stateMap.push_back(uploadMatches);
    stateMap.push_back(sleep);
}

void ShootoutGraph::build(const GameContext& ctx) {
    proposal = new ShootoutProposal(ctx);
    bracketReveal = new ShootoutBracketReveal(ctx);
    spectator = new ShootoutSpectator(ctx);
    eliminated = new ShootoutEliminated(ctx);
    finalStandings = new ShootoutFinalStandings(ctx);
    aborted = new ShootoutAborted(ctx);
}

void ShootoutGraph::wire(const CrossGraphTargets& cross) const {
    // Aliased for capture — see DuelGraph::wire.
    ShootoutProposal* proposal = this->proposal;
    ShootoutBracketReveal* bracketReveal = this->bracketReveal;
    ShootoutSpectator* spectator = this->spectator;
    ShootoutEliminated* eliminated = this->eliminated;
    ShootoutFinalStandings* finalStandings = this->finalStandings;
    ShootoutAborted* aborted = this->aborted;

    proposal->addTransition(
        new StateTransition(
            [proposal]() { return proposal->transitionToBracketReveal(); },
            bracketReveal));
    proposal->addTransition(
        new StateTransition(
            [proposal]() { return proposal->transitionToAborted(); },
            aborted));

    bracketReveal->addTransition(
        new StateTransition(
            [bracketReveal]() { return bracketReveal->transitionToDuelCountdown(); },
            cross.duelCountdown));
    bracketReveal->addTransition(
        new StateTransition(
            [bracketReveal]() { return bracketReveal->transitionToSpectator(); },
            spectator));
    bracketReveal->addTransition(
        new StateTransition(
            [bracketReveal]() { return bracketReveal->transitionToAborted(); },
            aborted));

    spectator->addTransition(
        new StateTransition(
            [spectator]() { return spectator->transitionToDuelCountdown(); },
            cross.duelCountdown));
    spectator->addTransition(
        new StateTransition(
            [spectator]() { return spectator->transitionToFinalStandings(); },
            finalStandings));
    spectator->addTransition(
        new StateTransition(
            [spectator]() { return spectator->transitionToAborted(); },
            aborted));

    eliminated->addTransition(
        new StateTransition(
            [eliminated]() { return eliminated->transitionToFinalStandings(); },
            finalStandings));
    eliminated->addTransition(
        new StateTransition(
            [eliminated]() { return eliminated->transitionToAborted(); },
            aborted));

    // Cable-event reset after TOURNAMENT_END: when the physical ring opens,
    // route through Sleep so the cooldown period elapses before the next
    // proposal can fire. Going straight to Idle let stale RDC chain state
    // (still advertising the old ring) feed back into CDM::isLoop on the
    // same loop tick as unplug, triggering a phantom Proposal with no peers
    // left to confirm.
    finalStandings->addTransition(
        new StateTransition(
            [finalStandings]() { return finalStandings->transitionToSleep(); },
            cross.sleep));

    aborted->addTransition(
        new StateTransition(
            [aborted]() { return aborted->transitionToIdle(); },
            cross.idle));
}

void ShootoutGraph::appendTo(std::vector<State*>& stateMap) const {
    stateMap.push_back(proposal);
    stateMap.push_back(bracketReveal);
    stateMap.push_back(spectator);
    stateMap.push_back(eliminated);
    stateMap.push_back(finalStandings);
    stateMap.push_back(aborted);
}

void SymbolGraph::build(const GameContext& ctx) {
    symbol = new SymbolState(ctx);
    symbolMatched = new SymbolMatched(ctx);
}

void SymbolGraph::wire(const CrossGraphTargets& cross) const {
    // Aliased for capture — see DuelGraph::wire.
    SymbolState* symbol = this->symbol;
    SymbolMatched* symbolMatched = this->symbolMatched;

    symbol->addTransition(
        new StateTransition(
            [symbol]() { return symbol->transitionToIdle(); },
            cross.idle));

    symbol->addTransition(
        new StateTransition(
            [symbol]() { return symbol->transitionToSymbolMatched(); },
            symbolMatched));

    symbolMatched->addTransition(
        new StateTransition(
            [symbolMatched]() { return symbolMatched->transitionToSymbol(); },
            symbol));

    symbolMatched->addTransition(
        new StateTransition(
            [symbolMatched]() { return symbolMatched->transitionToIdle(); },
            cross.idle));
}

void SymbolGraph::appendTo(std::vector<State*>& stateMap) const {
    stateMap.push_back(symbol);
    stateMap.push_back(symbolMatched);
}

SupporterReady* QuickdrawStateBuilder::build(std::vector<State*>& stateMap, const GameContext& ctx, RemoteDebugManager* remoteDebugManager) {
    DuelGraph duelGraph;
    ShootoutGraph shootoutGraph;
    SymbolGraph symbolGraph;
    duelGraph.build(ctx);
    shootoutGraph.build(ctx);
    symbolGraph.build(ctx);

    ShootoutManager* shootoutManager = ctx.shootoutManager;
    CrossGraphTargets cross;
    cross.shootoutProposal = shootoutGraph.proposal;
    cross.shootoutSpectator = shootoutGraph.spectator;
    cross.shootoutEliminated = shootoutGraph.eliminated;
    cross.shootoutAborted = shootoutGraph.aborted;
    cross.symbol = symbolGraph.symbol;
    cross.idle = duelGraph.idle;
    cross.duelCountdown = duelGraph.duelCountdown;
    cross.sleep = duelGraph.sleep;
    cross.phaseIsAborted = [shootoutManager]() {
        return shootoutManager && shootoutManager->getPhase() == ShootoutManager::Phase::ABORTED;
    };

    duelGraph.wire(ctx, cross);
    shootoutGraph.wire(cross);
    symbolGraph.wire(cross);

    PlayerRegistrationApp* playerRegistration = new PlayerRegistrationApp(ctx.player, ctx.wirelessManager, ctx.matchManager, remoteDebugManager);
    playerRegistration->addTransition(
        new StateTransition(
            [playerRegistration]() { return playerRegistration->readyForGameplay(); },
            duelGraph.awakenSequence));

    // Order matters: the first entry is the initial state.
    stateMap.push_back(playerRegistration);
    duelGraph.appendTo(stateMap);
    shootoutGraph.appendTo(stateMap);
    symbolGraph.appendTo(stateMap);

    return duelGraph.supporterReady;
}
