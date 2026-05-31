#include "wireless/reliable-channel.hpp"
#include "wireless/wireless-transport.hpp"
#include "device/wireless-manager.hpp"
#include "device/drivers/logger.hpp"

namespace {
constexpr const char* WTX_TAG = "WTX";
}

ReliableChannelBase::ReliableChannelBase(WirelessTransport* transport,
                                         Resender* resender,
                                         PktType type,
                                         uint8_t subType,
                                         OnAbandon onAbandon)
    : resender_(resender),
      type_(type),
      subType_(subType),
      transport_(transport),
      onAbandon_(std::move(onAbandon)) {}

void ReliableChannelBase::onAck(uint8_t seqId, const uint8_t* fromMac) {
    if (resender_) {
        resender_->onAck(type_, subType_, seqId, fromMac);
    }
}

void ReliableChannelBase::onResenderAbandon(uint8_t seqId, const uint8_t* targetMac) {
    if (onAbandon_) onAbandon_(seqId, targetMac);
}

bool ReliableChannelBase::isPending(const uint8_t* mac) const {
    return resender_ && resender_->isPending(type_, subType_, mac);
}

void ReliableChannelBase::cancel(const uint8_t* mac) {
    if (resender_) resender_->cancel(type_, subType_, mac);
}

uint8_t ReliableChannelBase::nextSeqId() {
    // 0 is reserved for "no ack expected" — skip it on wrap.
    lastSeqId_ = lastSeqId_ == 255 ? 1 : lastSeqId_ + 1;
    return lastSeqId_;
}

// Returning nullptr when transport_ is unset lets probe-style unit tests
// construct the base without standing up a full transport.
WirelessManager* ReliableChannelBase::getWirelessManager() const {
    return transport_ ? transport_->getWirelessManager() : nullptr;
}

void ReliableChannelBase::sendOnceBytes(const uint8_t* mac, const uint8_t* data, size_t len) {
    auto* wm = getWirelessManager();
    if (wm == nullptr) {
        LOG_E(WTX_TAG, "sendOnceBytes called with null WirelessManager");
        return;
    }
    wm->sendEspNowData(mac, type_, data, len);
}
