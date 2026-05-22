//
// EventTap Tests — pub/sub correctness for the simulator event sink.
//

#pragma once

#include <gtest/gtest.h>
#include <vector>
#include "cli/cli-event-tap.hpp"

class EventTapTestSuite : public testing::Test {
public:
    void SetUp() override {
        cli::EventTap::resetForTests();
    }
};

inline void eventTapDeliversPublishedEventsToSubscribers(EventTapTestSuite*) {
    std::vector<cli::SimEvent> captured;
    cli::EventTap::subscribe([&](const cli::SimEvent& e) { captured.push_back(e); });

    cli::SimEvent e;
    e.timestampMs = 1234;
    e.deviceIndex = 0;
    e.kind = "state_transition";
    e.kv.push_back({"from", "Idle"});
    e.kv.push_back({"to", "Handshake"});
    cli::EventTap::publish(e);

    ASSERT_EQ(captured.size(), 1u);
    EXPECT_EQ(captured[0].kind, "state_transition");
    EXPECT_EQ(captured[0].timestampMs, 1234u);
    EXPECT_EQ(captured[0].deviceIndex, 0);
    ASSERT_EQ(captured[0].kv.size(), 2u);
    EXPECT_EQ(captured[0].kv[0].first, "from");
    EXPECT_EQ(captured[0].kv[0].second, "Idle");
}

inline void eventTapDeliversToMultipleSubscribers(EventTapTestSuite*) {
    int countA = 0;
    int countB = 0;
    cli::EventTap::subscribe([&](const cli::SimEvent&) { countA++; });
    cli::EventTap::subscribe([&](const cli::SimEvent&) { countB++; });

    cli::SimEvent e;
    e.kind = "ping";
    cli::EventTap::publish(e);
    cli::EventTap::publish(e);

    EXPECT_EQ(countA, 2);
    EXPECT_EQ(countB, 2);
}

inline void eventTapSubscribeAfterPublishGetsOnlyFutureEvents(EventTapTestSuite*) {
    cli::SimEvent past;
    past.kind = "past";
    cli::EventTap::publish(past);

    std::vector<cli::SimEvent> captured;
    cli::EventTap::subscribe([&](const cli::SimEvent& e) { captured.push_back(e); });

    cli::SimEvent future;
    future.kind = "future";
    cli::EventTap::publish(future);

    ASSERT_EQ(captured.size(), 1u);
    EXPECT_EQ(captured[0].kind, "future");
}
