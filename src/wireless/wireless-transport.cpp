#include "wireless/wireless-transport.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "device/drivers/logger.hpp"

namespace {
const char* TAG = "WTX";
}

WirelessTransport::WirelessTransport(WirelessManager* wm)
    : wm_(wm), resender_(wm) {
    resender_.setAbandonCallback(
        [this](PktType type, uint8_t subType, uint8_t seqId,
               const uint8_t* targetMac) {
            onResenderAbandon(type, subType, seqId, targetMac);
        });
}

void WirelessTransport::onAckPacket(const uint8_t* from,
                                    const uint8_t* data, size_t len) {
    if (len < sizeof(AckPayload)) return;
    AckPayload ack;
    std::memcpy(&ack, data, sizeof(ack));
    PktType origType = static_cast<PktType>(ack.originalType);
    Key k = makeKey(origType, ack.subType);
    auto it = registry_.find(k);
    if (it == registry_.end()) return;
    it->second->onAck(ack.seqId, from);
}

bool WirelessTransport::deliverIncoming(PktType type, uint8_t subType,
                                        const uint8_t* fromMac,
                                        const uint8_t* data, size_t len) {
    Key k = makeKey(type, subType);
    auto it = registry_.find(k);
    if (it == registry_.end()) return false;
    return it->second->deliverBytes(fromMac, data, len);
}

void WirelessTransport::sync() {
    resender_.sync();
}

void WirelessTransport::onResenderAbandon(PktType type, uint8_t subType,
                                          uint8_t seqId,
                                          const uint8_t* targetMac) {
    Key k = makeKey(type, subType);
    auto it = registry_.find(k);
    if (it == registry_.end()) return;
    it->second->onResenderAbandon(seqId, targetMac);
}

void WirelessTransport::abortWithMessage(const char* msg) {
    LOG_E(TAG, "%s", msg);
    // Also write to stderr so death-test matchers (which scan child stderr)
    // can detect the abort message in native unit-test builds.
    std::fprintf(stderr, "%s\n", msg);
    std::abort();
}
