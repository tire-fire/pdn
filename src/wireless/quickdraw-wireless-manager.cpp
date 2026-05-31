//
// Created by Elli Furedy on 1/24/2025.
//
#include "wireless/quickdraw-wireless-manager.hpp"
#include "device/drivers/peer-comms-interface.hpp"


QuickdrawWirelessManager::QuickdrawWirelessManager() : broadcastTimer() {}

QuickdrawWirelessManager::~QuickdrawWirelessManager() {
    player = nullptr;
    wirelessManager = nullptr;
    // channel_ is owned by transport_; do not delete here.
}

void QuickdrawWirelessManager::initialize(Player *player, WirelessManager* wirelessManager,
                                          WirelessTransport* transport, long broadcastDelay) {
    this->player = player;
    this->broadcastDelay = broadcastDelay;
    this->wirelessManager = wirelessManager;
    this->transport_ = transport;
    if (transport_ == nullptr) {
        // Tests / native harness initialise without a transport. Reliable
        // sends collapse to no-ops in that mode (the FakeQuickdrawWireless-
        // Manager overrides broadcastReliable anyway).
        channel_ = nullptr;
        return;
    }
    channel_ = transport_->channel<QuickdrawPacket>(
        PktType::kQuickdrawCommand,
        [this](uint8_t seqId, const uint8_t* target) {
            LOG_W("QWM",
                "kQuickdrawCommand abandoned: target=%02X:%02X:%02X:%02X:%02X:%02X seqId=%u",
                target[0], target[1], target[2], target[3], target[4], target[5],
                (unsigned)seqId);
            if (abandonCallback_) abandonCallback_();
        });
    channel_->onReceive([this](const uint8_t* fromMac, const QuickdrawPacket& packet) {
        // Channel has already acked the inbound packet; we just dedupe and
        // dispatch (deliverDecoded skips the redundant ack when the entry
        // point is the channel).
        deliverDecoded(fromMac, packet);
    });
}

void QuickdrawWirelessManager::clearCallbacks() {
    packetReceivedCallback = nullptr;
    abandonCallback_ = nullptr;
}

void QuickdrawWirelessManager::setPacketReceivedCallback(const std::function<void(const QuickdrawCommand&)>& callback) {
    packetReceivedCallback = callback;
}

void QuickdrawWirelessManager::setAbandonCallback(AbandonCallback cb) {
    abandonCallback_ = std::move(cb);
}

void QuickdrawWirelessManager::sync() {
    // Drive the transport's Resender retransmits + abandon dispatch. Routed
    // through QWM so the per-tick call chain (Quickdraw::onStateLoop ->
    // qwm->sync) reaches the transport.
    if (transport_) transport_->sync();
}

int QuickdrawWirelessManager::broadcastPacket(const uint8_t macAddress[6],
                                             QuickdrawCommand& command) {
    QuickdrawPacket qdPacket = QuickdrawPacket();

    qdPacket.command = command.command;
    qdPacket.playerDrawTime = command.playerDrawTime;
    qdPacket.isHunter = command.isHunter;
    qdPacket.seqId = command.seqId;

    memcpy(qdPacket.matchId, command.matchId, IdGenerator::UUID_BUFFER_SIZE);
    memcpy(qdPacket.playerId, command.playerId, 5);

    LOG_I("QWM", "Sending command %i to %s", command.command, MacToString(macAddress));
    LOG_I("QWM", "Match ID: %s", qdPacket.matchId);
    LOG_I("QWM", "Player Draw Time: %ld", qdPacket.playerDrawTime);

    return wirelessManager->sendEspNowData(
        macAddress,
        PktType::kQuickdrawCommand,
        reinterpret_cast<const uint8_t*>(&qdPacket),
        sizeof(qdPacket));
}

int QuickdrawWirelessManager::broadcastReliable(const uint8_t macAddress[6],
                                                QuickdrawCommand& command) {
    if (channel_ == nullptr) return -1;

    QuickdrawPacket qdPacket = QuickdrawPacket();
    qdPacket.command = command.command;
    qdPacket.playerDrawTime = command.playerDrawTime;
    qdPacket.isHunter = command.isHunter;
    memcpy(qdPacket.matchId, command.matchId, IdGenerator::UUID_BUFFER_SIZE);
    memcpy(qdPacket.playerId, command.playerId, 5);

    uint8_t seqId = channel_->sendReliable(macAddress, qdPacket);
    command.seqId = seqId;

    LOG_I("QWM", "Sending reliable command %i to %s seqId=%u",
          command.command, MacToString(macAddress), (unsigned)seqId);
    return 0;
}

bool QuickdrawWirelessManager::isDuplicate(const uint8_t* mac, uint8_t command, uint8_t seqId) {
    for (auto& e : observed_) {
        if (e.command == command && memcmp(e.mac.data(), mac, 6) == 0) {
            if (e.lastSeqId == seqId) return true;
            e.lastSeqId = seqId;
            return false;
        }
    }
    ObservedKey k;
    memcpy(k.mac.data(), mac, 6);
    k.command = command;
    k.lastSeqId = seqId;
    observed_.push_back(k);
    return false;
}

void QuickdrawWirelessManager::sendAck(const uint8_t* toMac, uint8_t command, uint8_t seqId) {
    Resender::sendAck(wirelessManager, toMac, PktType::kQuickdrawCommand, command, seqId);
}

void QuickdrawWirelessManager::deliverDecoded(const uint8_t* fromMac,
                                              const QuickdrawPacket& packet) {
    if (packet.seqId != 0 &&
        isDuplicate(fromMac, static_cast<uint8_t>(packet.command), packet.seqId)) {
        return;
    }
    QuickdrawCommand command(fromMac, packet.command,
                             packet.matchId, packet.playerId,
                             packet.playerDrawTime, packet.isHunter,
                             packet.seqId);
    if (packetReceivedCallback) packetReceivedCallback(command);
}

int QuickdrawWirelessManager::processQuickdrawCommand(const uint8_t *macAddress, const uint8_t *data,
    const size_t dataLen) {

    if(dataLen != sizeof(QuickdrawPacket)) {
        LOG_E("QWM", "QuickdrawPacket size mismatch: got %lu, expected %lu (possible firmware mismatch)",
                      dataLen, sizeof(QuickdrawPacket));
        return -1;
    }

    const QuickdrawPacket* packet = reinterpret_cast<const QuickdrawPacket*>(data);

    // Legacy ingress path: ack inline so the sender stops retrying. The
    // channel-driven ingress path (transport->deliverIncoming -> channel
    // ->deliver) sends its own ack before calling deliverDecoded, so we
    // must NOT double-ack from this path when callers have already wired
    // packets through the transport. Tests and untransport'd devices use
    // this path exclusively.
    if (packet->seqId != 0) {
        sendAck(macAddress, static_cast<uint8_t>(packet->command), packet->seqId);
    }

    deliverDecoded(macAddress, *packet);
    return 1;
}
