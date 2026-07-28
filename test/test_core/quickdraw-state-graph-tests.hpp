#pragma once

#include <gtest/gtest.h>

#include <map>
#include <utility>
#include <vector>

#include "game/quickdraw-state-builder.hpp"
#include "apps/player-registration/player-registration-states.hpp"
#include "game/quickdraw-states.hpp"
#include "state/state.hpp"

// The graph these tests pin is otherwise unexecuted by the suite: the only
// Quickdraw a test builds never calls StateMachine::initialize(), so
// populateStateMap and QuickdrawStateBuilder::build never run. Both orders below
// are behaviour, not style — StateMachine::initialize mounts stateMap[0], and
// State::checkTransitions returns the first transition whose condition holds, so
// a reordering here silently reroutes the device while every other test stays
// green. The expectations were taken from the graph as it stood before the
// sub-graph split and must not be "corrected" to match a future change: if a
// change is meant to alter the graph, that intent belongs in the diff to these
// literals.
//
// A null GameContext is sufficient and deliberate. Every state constructor only
// stores its pointers, and reading a transition list does not evaluate its
// condition, so the shape can be inspected without standing up any manager.

namespace quickdraw_state_graph_expectations {

// Registration order. Index 0 is the state the device boots into.
inline const std::vector<int>& registrationOrder() {
    static const std::vector<int> ORDER = {
        PLAYER_REGISTRATION,
        AWAKEN_SEQUENCE,
        IDLE,
        SUPPORTER_READY,
        DUEL_COUNTDOWN,
        DUEL,
        DUEL_PUSHED,
        DUEL_RECEIVED_RESULT,
        DUEL_RESULT,
        WIN,
        LOSE,
        UPLOAD_MATCHES,
        SLEEP,
        SHOOTOUT_PROPOSAL,
        SHOOTOUT_BRACKET_REVEAL,
        SHOOTOUT_SPECTATOR,
        SHOOTOUT_ELIMINATED,
        SHOOTOUT_FINAL_STANDINGS,
        SHOOTOUT_ABORTED,
        SYMBOL,
        SYMBOL_MATCHED,
    };
    return ORDER;
}

// Per-state transition targets, in the order they were added. 48 edges.
inline const std::vector<std::pair<int, std::vector<int>>>& transitionOrder() {
    static const std::vector<std::pair<int, std::vector<int>>> EDGES = {
        {PLAYER_REGISTRATION, {AWAKEN_SEQUENCE}},
        {AWAKEN_SEQUENCE, {IDLE}},
        {IDLE,
         {SHOOTOUT_PROPOSAL, DUEL_COUNTDOWN, SUPPORTER_READY, SHOOTOUT_ABORTED, SYMBOL}},
        {SUPPORTER_READY, {IDLE}},
        {DUEL_COUNTDOWN, {SHOOTOUT_ABORTED, DUEL, IDLE}},
        {DUEL,
         {SHOOTOUT_ABORTED, SHOOTOUT_SPECTATOR, SHOOTOUT_ELIMINATED, IDLE,
          DUEL_RECEIVED_RESULT, DUEL_PUSHED}},
        {DUEL_PUSHED, {SHOOTOUT_ABORTED, IDLE, DUEL_RESULT}},
        {DUEL_RECEIVED_RESULT, {SHOOTOUT_ABORTED, IDLE, DUEL_RESULT}},
        {DUEL_RESULT,
         {SHOOTOUT_ABORTED, WIN, LOSE, SHOOTOUT_SPECTATOR, SHOOTOUT_ELIMINATED}},
        {WIN, {UPLOAD_MATCHES}},
        {LOSE, {UPLOAD_MATCHES}},
        {UPLOAD_MATCHES, {SLEEP}},
        {SLEEP, {AWAKEN_SEQUENCE}},
        {SHOOTOUT_PROPOSAL, {SHOOTOUT_BRACKET_REVEAL, SHOOTOUT_ABORTED}},
        {SHOOTOUT_BRACKET_REVEAL, {DUEL_COUNTDOWN, SHOOTOUT_SPECTATOR, SHOOTOUT_ABORTED}},
        {SHOOTOUT_SPECTATOR,
         {DUEL_COUNTDOWN, SHOOTOUT_FINAL_STANDINGS, SHOOTOUT_ABORTED}},
        {SHOOTOUT_ELIMINATED, {SHOOTOUT_FINAL_STANDINGS, SHOOTOUT_ABORTED}},
        {SHOOTOUT_FINAL_STANDINGS, {SLEEP}},
        {SHOOTOUT_ABORTED, {IDLE}},
        {SYMBOL, {IDLE, SYMBOL_MATCHED}},
        {SYMBOL_MATCHED, {SYMBOL, IDLE}},
    };
    return EDGES;
}

}  // namespace quickdraw_state_graph_expectations

// Builds the real graph with an empty context and hands back the state map.
// The caller owns every state, exactly as StateMachine does.
inline void buildQuickdrawGraphForTest(std::vector<State*>& stateMap) {
    GameContext ctx;
    QuickdrawStateBuilder::build(stateMap, ctx, nullptr);
}

inline void deleteQuickdrawGraphForTest(std::vector<State*>& stateMap) {
    for (State* state : stateMap)
        delete state;
    stateMap.clear();
}

// stateMap[0] is what StateMachine::initialize mounts, so this pins the boot
// state as much as it pins the order. skipToState is index-addressed too, and
// two CLI call sites pass a literal 1.
inline void quickdrawGraphRegistrationOrderIsPinned() {
    std::vector<State*> stateMap;
    buildQuickdrawGraphForTest(stateMap);

    const std::vector<int>& expected =
        quickdraw_state_graph_expectations::registrationOrder();
    ASSERT_EQ(stateMap.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(stateMap[i]->getStateId(), expected[i])
            << "registration slot " << i << " changed";
    }

    deleteQuickdrawGraphForTest(stateMap);
}

// Transition position is priority: checkTransitions returns the first condition
// that holds, so demoting an edge below a sibling that can be true at the same
// moment silently disables it.
inline void quickdrawGraphTransitionOrderIsPinned() {
    std::vector<State*> stateMap;
    buildQuickdrawGraphForTest(stateMap);

    std::map<int, State*> byId;
    for (State* state : stateMap)
        byId[state->getStateId()] = state;

    size_t totalEdges = 0;
    for (const std::pair<int, std::vector<int>>& expected :
         quickdraw_state_graph_expectations::transitionOrder()) {
        State* source = byId.count(expected.first) ? byId[expected.first] : nullptr;
        ASSERT_NE(source, nullptr) << "state " << expected.first << " missing";

        const std::vector<StateTransition*>& actual = source->getTransitions();
        ASSERT_EQ(actual.size(), expected.second.size())
            << "state " << expected.first << " transition count changed";
        for (size_t i = 0; i < expected.second.size(); ++i) {
            ASSERT_NE(actual[i]->getNextState(), nullptr)
                << "state " << expected.first << " edge " << i << " targets null";
            EXPECT_EQ(actual[i]->getNextState()->getStateId(), expected.second[i])
                << "state " << expected.first << " edge " << i << " retargeted";
        }
        totalEdges += expected.second.size();
    }
    EXPECT_EQ(totalEdges, 48u);

    deleteQuickdrawGraphForTest(stateMap);
}
