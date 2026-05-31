#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <utility>

#include "wireless/peer-comms-types.hpp"
#include "wireless/reliable-channel.hpp"
#include "wireless/resender.hpp"

class WirelessManager;

// Owns one Resender. Vends typed channels keyed by (PktType, subType).
// All channels share the single Resender; abandon callbacks fire inline
// during sync() and must be cheap.
class WirelessTransport {
public:
    using Stats = Resender::Stats;

    explicit WirelessTransport(WirelessManager* wm);

    // Construct a channel for (PktType, subType=0). Aborts (loud failure)
    // if another channel has already claimed this (PktType, subType).
    template <class P>
    ReliableChannel<P>* channel(PktType type,
                                ReliableChannelBase::OnAbandon onAbandon) {
        return channelImpl<P, void>(type, 0, std::move(onAbandon));
    }

    // Construct a channel for (PktType, subType). Sub may be an enum class
    // (e.g., ShootoutCmd); its underlying uint8_t value is the subType.
    template <class P, class Sub>
    ReliableChannel<P, Sub>* channel(PktType type, Sub subType,
                                     ReliableChannelBase::OnAbandon onAbandon) {
        return channelImpl<P, Sub>(type, static_cast<uint8_t>(subType),
                                   std::move(onAbandon));
    }

    // Routes the inbound AckPayload to the owning channel, if any.
    void onAckPacket(const uint8_t* from, const uint8_t* data, size_t len);

    // Dispatches an inbound data packet to the channel claiming
    // (type, subType). Returns false if no channel is registered.
    bool deliverIncoming(PktType type, uint8_t subType,
                         const uint8_t* fromMac, const uint8_t* data, size_t len);

    // Called every loop tick. Drives Resender's retransmits and abandon
    // dispatch.
    void sync();

    Stats getStats(PktType type, uint8_t subType = 0) {
        return resender_.getStats(type, subType);
    }

    WirelessManager* getWirelessManager() { return wm_; }

private:
    using Key = uint32_t; // (subType << 16) | type

    template <class P, class Sub>
    ReliableChannel<P, Sub>* channelImpl(PktType type, uint8_t subType,
                                         ReliableChannelBase::OnAbandon onAbandon) {
        Key k = makeKey(type, subType);
        auto channel = std::make_unique<ReliableChannel<P, Sub>>(
            this, &resender_, type, subType, std::move(onAbandon));
        auto* raw = channel.get();
        auto [it, inserted] = registry_.emplace(k, std::move(channel));
        if (!inserted) {
            abortWithMessage("duplicate channel claim");
        }
        return raw;
    }

    static Key makeKey(PktType type, uint8_t subType) {
        return (static_cast<Key>(subType) << 16) |
               static_cast<Key>(static_cast<uint8_t>(type));
    }

    [[noreturn]] static void abortWithMessage(const char* msg);

    void onResenderAbandon(PktType type, uint8_t subType,
                           uint8_t seqId, const uint8_t* targetMac);

    WirelessManager* wm_;
    Resender resender_;
    std::map<Key, std::unique_ptr<ReliableChannelBase>> registry_;
};
