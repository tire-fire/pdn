#pragma once

#include <cstdint>
#include <cstring>
#include <functional>

#include "wireless/peer-comms-types.hpp"
#include "wireless/resender.hpp"

class WirelessTransport;
class WirelessManager;

class ReliableChannelBase {
public:
    using OnAbandon = std::function<void(uint8_t seqId, const uint8_t* targetMac)>;

    ReliableChannelBase(WirelessTransport* transport,
                        Resender* resender,
                        PktType type,
                        uint8_t subType,
                        OnAbandon onAbandon,
                        Resender::SendMode sendMode =
                            Resender::SendMode::SupersedePerTarget);
    virtual ~ReliableChannelBase() = default;

    PktType type() const { return type_; }
    uint8_t subType() const { return subType_; }

    void onAck(uint8_t seqId, const uint8_t* fromMac);
    void onResenderAbandon(uint8_t seqId, const uint8_t* targetMac);

    bool isPending(const uint8_t* mac) const;
    void cancel(const uint8_t* mac);

    // Pure virtual: typed subclass deserializes bytes into P and dispatches
    // to its onReceive callback. Returns true if delivered.
    virtual bool deliverBytes(const uint8_t* fromMac, const uint8_t* data, size_t len) = 0;

protected:
    uint8_t nextSeqId();
    // Helper for the template subclass; defined in reliable-channel.cpp where
    // WirelessTransport is complete.
    WirelessManager* getWirelessManager() const;
    void sendOnceBytes(const uint8_t* mac, const uint8_t* data, size_t len);
    // Defined in the .cpp so the logger stays out of this template header.
    static void logLengthMismatch(PktType type, uint8_t subType,
                                  size_t got, size_t want);

    Resender* resender_;
    PktType type_;
    uint8_t subType_;
    WirelessTransport* transport_;
    Resender::SendMode sendMode_;

private:
    OnAbandon onAbandon_;
    uint8_t lastSeqId_ = 0;
};

template <class P, class Sub = void>
class ReliableChannel : public ReliableChannelBase {
public:
    using OnReceive = std::function<void(const uint8_t* fromMac, const P&)>;

    ReliableChannel(WirelessTransport* transport,
                    Resender* resender,
                    PktType type,
                    uint8_t subType,
                    OnAbandon onAbandon,
                    Resender::SendMode sendMode =
                        Resender::SendMode::SupersedePerTarget)
        : ReliableChannelBase(transport, resender, type, subType,
                              std::move(onAbandon), sendMode) {}

    uint8_t sendReliable(const uint8_t* mac, P p) {
        p.seqId = nextSeqId();
        resender_->send(mac, type_, subType_, p.seqId,
                        reinterpret_cast<const uint8_t*>(&p), sizeof(P), sendMode_);
        return p.seqId;
    }

    void sendOnce(const uint8_t* mac, P p) {
        sendOnceBytes(mac, reinterpret_cast<const uint8_t*>(&p), sizeof(P));
    }

    bool deliver(const uint8_t* fromMac, const uint8_t* data, size_t len) {
        if (len != sizeof(P)) {
            // Drop without acking: a corrupt/truncated ESP-NOW frame (no CRC on
            // this path) would otherwise be retransmitted to abandonment with no
            // trace. Log so it's diagnosable rather than silent.
            logLengthMismatch(type_, subType_, len, sizeof(P));
            return false;
        }
        P p;
        std::memcpy(&p, data, sizeof(P));
        Resender::sendAck(getWirelessManager(), fromMac, type_, subType_, p.seqId);
        if (onReceive_) onReceive_(fromMac, p);
        return true;
    }

    bool deliverBytes(const uint8_t* fromMac, const uint8_t* data, size_t len) override {
        return deliver(fromMac, data, len);
    }

    void onReceive(OnReceive cb) { onReceive_ = std::move(cb); }

private:
    OnReceive onReceive_;
};
