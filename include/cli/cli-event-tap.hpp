#pragma once

#ifdef NATIVE_BUILD

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include "utils/simple-timer.hpp"

namespace cli {

inline uint32_t eventTimestampMs() {
    auto* clk = SimpleTimer::getPlatformClock();
    return clk ? (uint32_t)clk->milliseconds() : 0;
}

struct SimEvent {
    uint32_t timestampMs = 0;
    int deviceIndex = -1;
    std::string kind;
    std::vector<std::pair<std::string, std::string>> kv;
};

class EventTap {
public:
    using Subscriber = std::function<void(const SimEvent&)>;

    static void subscribe(Subscriber s) {
        std::lock_guard<std::mutex> lock(mutex());
        subscribers().push_back(std::move(s));
    }

    static void publish(const SimEvent& e) {
        // Snapshot under lock then iterate unlocked so subscribers may publish (re-entrance).
        std::vector<Subscriber> snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex());
            snapshot = subscribers();
        }
        for (auto& s : snapshot) {
            s(e);
        }
    }

    static void resetForTests() {
        std::lock_guard<std::mutex> lock(mutex());
        subscribers().clear();
    }

private:
    static std::vector<Subscriber>& subscribers() {
        static std::vector<Subscriber> s;
        return s;
    }
    static std::mutex& mutex() {
        static std::mutex m;
        return m;
    }
};

} // namespace cli

#endif // NATIVE_BUILD
