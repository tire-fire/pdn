//
// Created by Elli Furedy on 1/24/2025.
//
#include "wireless/quickdraw-wireless-manager.hpp"
#include "device/drivers/peer-comms-interface.hpp"

namespace {
// Common QuickdrawCommand -> QuickdrawPacket field copy. seqId is left for the
// caller: the fire-and-forget path carries the command's own seqId, the
// reliable path has it assigned by the channel on send.
QuickdrawPacket encodeCommand(const QuickdrawCommand& command) {
    QuickdrawPacket p = QuickdrawPacket();
    p.command = command.command;
    p.playerDrawTime = command.playerDrawTime;
    p.isHunter = command.isHunter;
    memcpy(p.matchId, command.matchId, IdGenerator::UUID_BUFFER_SIZE);
    memcpy(p.playerId, command.playerId, 5);
    return p;
}
}

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
    // Inbound kQuickdrawCommand bytes are routed (by main.cpp / cli-device /
    // tests) through transport->deliverIncoming, which acks, drops the lost-ack
    // duplicate, then fires this onReceive. One ack+dedup path, shared with
    // every other reliable channel.
    channel_->onReceive(
        [this](const uint8_t* fromMac, const QuickdrawPacket& packet) {
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
    QuickdrawPacket qdPacket = encodeCommand(command);
    qdPacket.seqId = command.seqId;

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

    QuickdrawPacket qdPacket = encodeCommand(command);

    uint8_t seqId = channel_->sendReliable(macAddress, qdPacket);
    command.seqId = seqId;

    LOG_I("QWM", "Sending reliable command %i to %s seqId=%u",
          command.command, MacToString(macAddress), (unsigned)seqId);
    return 0;
}

void QuickdrawWirelessManager::deliverDecoded(const uint8_t* fromMac,
                                              const QuickdrawPacket& packet) {
    QuickdrawCommand command(fromMac, packet.command,
                             packet.matchId, packet.playerId,
                             packet.playerDrawTime, packet.isHunter,
                             packet.seqId);
    if (packetReceivedCallback) packetReceivedCallback(command);
}
