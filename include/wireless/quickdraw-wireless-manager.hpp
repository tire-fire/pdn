#pragma once

//
// Created by Elli Furedy on 1/21/2025.
//
#include <vector>
#include <cstring>  // For memcpy
#include <functional>
#include <map>
#include <cstdint>
#include "utils/simple-timer.hpp"
#include "game/player.hpp"
#include "game/match.hpp"
#include "id-generator.hpp"
#include "mac-functions.hpp"
#include "device/wireless-manager.hpp"
#include "wireless/resender.hpp"
#include "wireless/reliable-channel.hpp"
#include "wireless/wireless-transport.hpp"

// Wire format transmitted over ESP-NOW for every quickdraw command.
// Defined here so tests can construct and inspect packets without duplicating the layout.
// seqId is used for Resender's ack matching; 0 means "fire-and-forget, no ack expected".
struct QuickdrawPacket {
    char matchId[37];  // IdGenerator::UUID_BUFFER_SIZE
    char playerId[5];  // 4 chars + null terminator
    bool isHunter;
    long playerDrawTime;
    int  command;
    uint8_t seqId;
} __attribute__((packed));

enum QDCommand {
    // Game Commands
    // HACK = 6,
    // HACK_ACK = 7, 
    // HACK_CONFIRMED = 8,
    // LOCKDOWN = 9,
    // LOCKDOWN_ACK = 10,
    // LOCKDOWN_CONFIRMED = 11,    
    SEND_MATCH_ID = 6,
    MATCH_ID_ACK = 7,
    MATCH_ROLE_MISMATCH = 8,
    DRAW_RESULT = 9,
    NEVER_PRESSED = 10,
    COMMAND_COUNT,  // Always add new commands above this line
    INVALID_COMMAND = 0xFF
};

struct QuickdrawCommand {
    const uint8_t* wifiMacAddr;
    int command;
    char matchId[IdGenerator::UUID_BUFFER_SIZE];
    char playerId[5];  // 4 chars + null terminator
    bool isHunter;
    long playerDrawTime;
    uint8_t seqId;

    QuickdrawCommand(const uint8_t* macAddress, int command, const char* matchId, const char* playerId, long playerDrawTime, bool isHunter, uint8_t seqId = 0)
        : command(command), playerDrawTime(playerDrawTime), isHunter(isHunter), seqId(seqId) {
        this->wifiMacAddr = macAddress;

        memcpy(this->matchId, matchId, IdGenerator::UUID_BUFFER_SIZE);

        memcpy(this->playerId, playerId, 5);
    }
};

using QDCommandTracker = std::map<int, QuickdrawCommand>;

class QuickdrawWirelessManager {
public:
    QuickdrawWirelessManager();
    virtual ~QuickdrawWirelessManager();

    // Vends a ReliableChannel<QuickdrawPacket> on (kQuickdrawCommand, 0) from
    // the supplied transport and binds its onReceive. Sends route via the
    // channel; inbound bytes arrive through transport->deliverIncoming, which
    // acks and dedups before dispatching to the packet callback.
    void initialize(Player* player, WirelessManager* wirelessManager,
                    WirelessTransport* transport, long broadcastCooldown);
    // Transport-free overload — passing nullptr transport keeps the manager usable
    // for tests/devices that do not stand up a WirelessTransport. Reliable
    // sends in this mode return -1.
    void initialize(Player* player, WirelessManager* wirelessManager, long broadcastCooldown) {
        initialize(player, wirelessManager, nullptr, broadcastCooldown);
    }

    void setPacketReceivedCallback(const std::function<void(const QuickdrawCommand&)>& callback);

    // The transport whose kQuickdrawCommand channel acks, dedups, and dispatches
    // inbound packets. Callers route received bytes through
    // transport->deliverIncoming(kQuickdrawCommand, 0, ...); exposed so test
    // harnesses can inject inbound frames at the same boundary.
    WirelessTransport* getTransport() const { return transport_; }

    // Fire-and-forget send for commands with their own recovery
    // (matchInitializationTimer in Idle handles SEND_MATCH_ID etc.).
    virtual int broadcastPacket(const uint8_t* macAddress,
                               QuickdrawCommand& command);

    // Reliable send via Resender; assigns seqId. Used for DRAW_RESULT /
    // NEVER_PRESSED, where packet loss would corrupt real match results.
    virtual int broadcastReliable(const uint8_t* macAddress,
                                  QuickdrawCommand& command);

    // Signals that a reliable send to a peer exhausted its retry budget. The
    // only reliable commands are DRAW_RESULT and NEVER_PRESSED, and the sender
    // already knows which it sent (whether it pressed), so no per-send context
    // is threaded back.
    using AbandonCallback = std::function<void()>;
    void setAbandonCallback(AbandonCallback cb);

    // Drop any in-flight DRAW_RESULT / NEVER_PRESSED to the given peer.
    // Without this, a stale abandon arriving after the match clears could
    // void the next match primed against the same peer.
    void cancelPendingReliable(const uint8_t* target) {
        if (channel_ == nullptr || target == nullptr) return;
        channel_->cancel(target);
    }

    // Must be called every loop tick to drive Resender retransmits.
    void sync();

    void clearCallbacks();

private:
    WirelessManager* wirelessManager;

    std::function<void(const QuickdrawCommand&)> packetReceivedCallback;
    AbandonCallback abandonCallback_;

    Player* player;

    QDCommandTracker commandTracker;

    SimpleTimer broadcastTimer;

    long broadcastDelay;

    WirelessTransport* transport_ = nullptr;
    ReliableChannel<QuickdrawPacket>* channel_ = nullptr;

    // Decode a received packet into a QuickdrawCommand and hand it to the
    // packet callback. Bound as the channel's onReceive, which has already
    // acked and dropped the lost-ack duplicate before calling this.
    void deliverDecoded(const uint8_t* fromMac, const QuickdrawPacket& packet);
};
