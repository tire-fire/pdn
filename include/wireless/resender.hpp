#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <type_traits>
#include <vector>

#include "utils/simple-timer.hpp"
#include "wireless/peer-comms-types.hpp"

class WirelessManager;

// Reliable unicast: send a packet to a peer, retransmit on timeout, abandon
// after maxRetries. Per-target pending granularity depends on SendMode: state
// channels keep one entry per (PktType, subType, target); stream channels keep
// one per (PktType, subType, target, seqId) so a batch of distinct packets to
// the same peer all retain their own retry slots. sync() must be called every
// loop tick to drive retransmits.
class Resender {
public:
    // How a send relates to other in-flight sends to the same peer on the same
    // channel. SupersedePerTarget (default): the payload is current state, so a
    // newer send obsoletes any prior unacked one and only the latest survives
    // (an older retransmit arriving last would otherwise reinstate stale state).
    // KeepDistinct: the payload is one item of a stream (e.g. a bracket slot),
    // so each seqId keeps its own retry slot and a dropped one still retransmits.
    enum class SendMode { SupersedePerTarget, KeepDistinct };

    struct RetryPolicy {
        unsigned long initialTimeoutMs = 100;
        uint8_t maxRetries = 3;
        // exponential: 100, 200, 400 ... ; non-exponential: constant initialTimeoutMs.
        bool exponentialBackoff = true;
    };

    struct Stats {
        uint32_t sends = 0;
        uint32_t retries = 0;
        uint32_t abandons = 0;
        uint32_t ackLatencyMsSum = 0;
        uint32_t ackCount = 0;
    };

    // Fires once per pending entry that exhausts its retry budget. Invoked
    // from sync() AFTER all retransmits have been processed and the
    // abandoned entries removed, so callbacks may freely call send() or
    // cancel() on this Resender without invalidating the iteration.
    using AbandonCallback = std::function<void(PktType type, uint8_t subType,
                                               uint8_t seqId,
                                               const uint8_t* targetMac)>;

    explicit Resender(WirelessManager* wirelessManager)
        : wm_(wirelessManager) {}
    ~Resender() = default;

    // Send a kAck for a reliably-received packet. All subsystems share
    // this one ack wire format.
    static void sendAck(WirelessManager* wm, const uint8_t* toMac,
                        PktType originalType, uint8_t subType, uint8_t seqId);
    template <class C, class = std::enable_if_t<std::is_enum_v<C>>>
    static void sendAck(WirelessManager* wm, const uint8_t* toMac,
                        PktType originalType, C channel, uint8_t seqId) {
        sendAck(wm, toMac, originalType, toSubType(channel), seqId);
    }

    void setAbandonCallback(AbandonCallback cb) {
        abandonCallback_ = std::move(cb);
    }

    template <class C,
              class = std::enable_if_t<std::is_enum_v<C> || std::is_integral_v<C>>>
    static constexpr uint8_t toSubType(C c) {
        return static_cast<uint8_t>(c);
    }

    // Reliable send. SendMode controls how it relates to other pending sends to
    // the same (type, subType, target): SupersedePerTarget drops any prior one,
    // KeepDistinct keeps prior sends with a different seqId. payload bytes are
    // copied.
    void send(const uint8_t* target, PktType type, uint8_t subType,
              uint8_t seqId, const uint8_t* payload, size_t len,
              SendMode mode = SendMode::SupersedePerTarget);
    template <class C, class = std::enable_if_t<std::is_enum_v<C>>>
    void send(const uint8_t* target, PktType type, C channel,
              uint8_t seqId, const uint8_t* payload, size_t len) {
        send(target, type, toSubType(channel), seqId, payload, len);
    }
    void send(const uint8_t* target, PktType type, uint8_t seqId,
              const uint8_t* payload, size_t len) {
        send(target, type, uint8_t{0}, seqId, payload, len);
    }

    bool onAck(PktType type, uint8_t subType, uint8_t seqId, const uint8_t* fromMac);
    template <class C, class = std::enable_if_t<std::is_enum_v<C>>>
    bool onAck(PktType type, C channel, uint8_t seqId, const uint8_t* fromMac) {
        return onAck(type, toSubType(channel), seqId, fromMac);
    }
    bool onAck(PktType type, uint8_t seqId, const uint8_t* fromMac) {
        return onAck(type, uint8_t{0}, seqId, fromMac);
    }

    // Silent drop; use when the target is known unreachable. No abandon callback.
    void cancel(PktType type, uint8_t subType, const uint8_t* target);
    template <class C, class = std::enable_if_t<std::is_enum_v<C>>>
    void cancel(PktType type, C channel, const uint8_t* target) {
        cancel(type, toSubType(channel), target);
    }
    void cancel(PktType type, const uint8_t* target) {
        cancel(type, uint8_t{0}, target);
    }

    // Must be called every loop tick.
    void sync();

    Stats getStats(PktType type, uint8_t subType = 0) const {
        auto it = perChannelStats_.find(channelKey(type, subType));
        if (it == perChannelStats_.end()) return Stats{};
        return it->second;
    }

    size_t pendingCount() const { return pending_.size(); }
    size_t pendingCount(PktType type, uint8_t subType = 0) const {
        size_t count = 0;
        for (const auto& p : pending_) {
            if (p.type == type && p.subType == subType) ++count;
        }
        return count;
    }
    template <class C, class = std::enable_if_t<std::is_enum_v<C>>>
    size_t pendingCount(PktType type, C channel) const {
        return pendingCount(type, toSubType(channel));
    }

    bool isPending(PktType type, uint8_t subType, const uint8_t* target) const {
        if (target == nullptr) return false;
        for (const auto& p : pending_) {
            if (p.type != type || p.subType != subType) continue;
            if (memcmp(p.target.data(), target, 6) == 0) return true;
        }
        return false;
    }
    template <class C, class = std::enable_if_t<std::is_enum_v<C>>>
    bool isPending(PktType type, C channel, const uint8_t* target) const {
        return isPending(type, toSubType(channel), target);
    }

private:
    struct Pending {
        PktType type;
        uint8_t subType;
        std::array<uint8_t, 6> target;
        uint8_t seqId;
        std::vector<uint8_t> payload;
        uint8_t retries;
        RetryPolicy policy;
        SimpleTimer timer;
    };

    static unsigned long backoffMs(const RetryPolicy& p, uint8_t retryNum) {
        if (!p.exponentialBackoff) return p.initialTimeoutMs;
        // Clamp the shift: maxRetries is a caller-settable uint8_t and unsigned
        // long is 32-bit on the ESP32, so an unclamped shift would be UB / wrap
        // past ~25 doublings. 16 (~65000x base) is far beyond any sane budget.
        unsigned shift = retryNum > 16 ? 16u : retryNum;
        return p.initialTimeoutMs << shift;
    }

    Stats& statsFor(PktType type, uint8_t subType) {
        return perChannelStats_[channelKey(type, subType)];
    }

    std::vector<Pending>::iterator findPending(
        PktType type, uint8_t subType, uint8_t seqId, const uint8_t* target);

    // Drop every pending entry to `target` on (type, subType), across all seqIds.
    void eraseAllToTarget(PktType type, uint8_t subType, const uint8_t* target);

    void transmit(const Pending& p);

    WirelessManager* wm_;
    std::vector<Pending> pending_;
    std::map<ChannelKey, Stats> perChannelStats_;
    AbandonCallback abandonCallback_;
};
