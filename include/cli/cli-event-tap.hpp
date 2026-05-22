#pragma once

#ifdef NATIVE_BUILD

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace cli {

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
        std::vector<Subscriber> snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex());
            snapshot = subscribers();
        }
        for (auto& s : snapshot) {
            s(e);
        }
    }

    // Test-only: reset subscriber list between test cases.
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
