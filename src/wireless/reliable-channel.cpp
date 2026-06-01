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
                                         OnAbandon onAbandon,
                                         Resender::SendMode sendMode)
    : resender_(resender),
      type_(type),
      subType_(subType),
      transport_(transport),
      sendMode_(sendMode),
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

bool ReliableChannelBase::isDuplicateReliableRx(const uint8_t* fromMac, uint8_t seqId) {
    if (seqId == 0 || fromMac == nullptr) return false;
    for (auto& r : rxSeq_) {
        if (std::memcmp(r.mac.data(), fromMac, 6) == 0) {
            if (r.lastSeqId == seqId) return true;
            r.lastSeqId = seqId;
            return false;
        }
    }
    RxSeqRecord rec;
    std::memcpy(rec.mac.data(), fromMac, 6);
    rec.lastSeqId = seqId;
    rxSeq_.push_back(rec);
    return false;
}

void ReliableChannelBase::logLengthMismatch(PktType type, uint8_t subType,
                                            size_t got, size_t want) {
    LOG_W(WTX_TAG, "reliable rx len mismatch type=%u sub=%u got=%zu want=%zu",
          (unsigned)type, (unsigned)subType, got, want);
}

void ReliableChannelBase::sendOnceBytes(const uint8_t* mac, const uint8_t* data, size_t len) {
    auto* wm = getWirelessManager();
    if (wm == nullptr) {
        LOG_E(WTX_TAG, "sendOnceBytes called with null WirelessManager");
        return;
    }
    wm->sendEspNowData(mac, type_, data, len);
}
