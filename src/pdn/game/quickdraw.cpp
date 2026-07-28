#include "game/quickdraw.hpp"
#include "game/quickdraw-state-builder.hpp"
#include "device/drivers/peer-comms-types.hpp"
#include "wireless/symbol-wireless-manager.hpp"
#include "device/drivers/logger.hpp"
#include <array>
#include <cstring>

Quickdraw::Quickdraw(Player* player, Device* PDN, QuickdrawWirelessManager* quickdrawWirelessManager, RemoteDebugManager* remoteDebugManager, SymbolWirelessManager* symbolWirelessManager): StateMachine(QUICKDRAW_APP_ID) {
    this->player = player;
    this->quickdrawWirelessManager = quickdrawWirelessManager;
    this->symbolWirelessManager = symbolWirelessManager;
    this->remoteDebugManager = remoteDebugManager;
    this->wirelessManager = PDN->getWirelessManager();
    this->matchManager = new MatchManager();
    this->storageManager = PDN->getStorage();
    this->peerComms = PDN->getPeerComms();
    this->remoteDeviceCoordinator = PDN->getRemoteDeviceCoordinator();

    this->chainDuelManager = new ChainDuelManager(player, wirelessManager, remoteDeviceCoordinator);
    // Reads chainDuelManager, which this constructor assigns in the body: hoisting
    // this into the member-initializer list would read it uninitialized.
    this->shootoutManager = new ShootoutManager(player, wirelessManager, remoteDeviceCoordinator, chainDuelManager);  // NOLINT(cppcoreguidelines-prefer-member-initializer)
    this->shootoutManager->setMatchManager(matchManager);
    matchManager->setShootoutManager(shootoutManager);

    matchManager->initialize(player, storageManager, quickdrawWirelessManager);
    matchManager->setBoostProvider([this]() -> unsigned long {
        return chainDuelManager ? chainDuelManager->getBoostMs() : 0;
    });
    matchManager->setRemoteDeviceCoordinator(remoteDeviceCoordinator);

    quickdrawWirelessManager->setPacketReceivedCallback(
        std::bind(&MatchManager::listenForMatchEvents, matchManager, std::placeholders::_1)
    );

    // Chain game event + confirm wireless packet handlers.
    wirelessManager->setEspNowPacketHandler(
        PktType::kChainGameEvent,
        [](const uint8_t* macAddress, const uint8_t* data, const size_t dataLen, void* ctx) {
            static_cast<Quickdraw*>(ctx)->onChainGameEventPacket(macAddress, data, dataLen);
        },
        this
    );
    wirelessManager->setEspNowPacketHandler(
        PktType::kChainGameEventAck,
        [](const uint8_t* macAddress, const uint8_t* data, const size_t dataLen, void* ctx) {
            static_cast<Quickdraw*>(ctx)->onChainGameEventAckPacket(macAddress, data, dataLen);
        },
        this
    );
    wirelessManager->setEspNowPacketHandler(
        PktType::kChainConfirm,
        [](const uint8_t* macAddress, const uint8_t* data, const size_t dataLen, void* ctx) {
            static_cast<Quickdraw*>(ctx)->onChainConfirmPacket(macAddress, data, dataLen);
        },
        this
    );
    wirelessManager->setEspNowPacketHandler(
        PktType::kRoleAnnounce,
        [](const uint8_t* macAddress, const uint8_t* data, const size_t dataLen, void* ctx) {
            static_cast<Quickdraw*>(ctx)->onRoleAnnouncePacket(macAddress, data, dataLen);
        },
        this
    );
    wirelessManager->setEspNowPacketHandler(
        PktType::kRoleAnnounceAck,
        [](const uint8_t* macAddress, const uint8_t* data, const size_t dataLen, void* ctx) {
            static_cast<Quickdraw*>(ctx)->onRoleAnnounceAckPacket(macAddress, data, dataLen);
        },
        this
    );
    wirelessManager->setEspNowPacketHandler(
        PktType::kShootoutCommand,
        [](const uint8_t* macAddress, const uint8_t* data, const size_t dataLen, void* ctx) {
            static_cast<Quickdraw*>(ctx)->onShootoutCommandPacket(macAddress, data, dataLen);
        },
        this
    );
    wirelessManager->setEspNowPacketHandler(
        PktType::kShootoutCommandAck,
        [](const uint8_t* macAddress, const uint8_t* data, const size_t dataLen, void* ctx) {
            static_cast<Quickdraw*>(ctx)->onShootoutCommandAckPacket(macAddress, data, dataLen);
        },
        this
    );

    if (symbolWirelessManager) {
        symbolWirelessManager->initialize(wirelessManager, remoteDeviceCoordinator);
        wirelessManager->setEspNowPacketHandler(
            PktType::kSymbolMatchCommand,
            [](const uint8_t* macAddress, const uint8_t* data, const size_t dataLen, void* ctx) {
                static_cast<SymbolWirelessManager*>(ctx)->processSymbolMatchCommand(macAddress, data, dataLen);
            },
            symbolWirelessManager
        );
    }

    // Clear boost/confirmed-supporters when the supporter chain drains to
    // empty while a duel is still running. Without this, a champion keeps
    // a boost from supporters that have since unplugged.
    remoteDeviceCoordinator->setChainChangeCallback([this]() {
        onChainStateChanged();
    });

    remoteDeviceCoordinator->setPeerLostCallback([this](const uint8_t* lostMac) {
        if (shootoutManager && shootoutManager->active()) {
            shootoutManager->onLocalRDCDisconnect(lostMac);
        }
    });

    // The Player is the authority on this device's identity; RDC only carries it.
    remoteDeviceCoordinator->setSelfProfileProvider([this]() -> PlayerProfile {
        return this->player->toProfile();
    });

    // Registration flips hunter/bounty long after the jacks came up, and the
    // context exchange only runs on connect, so the flip has to push itself.
    this->player->setOnRoleChanged([this]() {
        remoteDeviceCoordinator->resendContext();
    });
}

void Quickdraw::onChainStateChanged() {
    if (chainDuelManager) {
        chainDuelManager->onChainStateChanged();
    }
    // Shootout disconnects flow through setPeerLostCallback, not chain-state
    // diffs — daisy chain announcements bounce in normal operation.
}

void Quickdraw::onRoleAnnouncePacket(const uint8_t* fromMac, const uint8_t* data, size_t dataLen) {
    if (dataLen != sizeof(RoleAnnouncePayload) || !chainDuelManager) return;
    const RoleAnnouncePayload* payload = reinterpret_cast<const RoleAnnouncePayload*>(data);
    chainDuelManager->onRoleAnnounceReceived(
        fromMac, payload->role, payload->championMac, payload->seqId);
}

void Quickdraw::onRoleAnnounceAckPacket(const uint8_t* fromMac, const uint8_t* data, size_t dataLen) {
    if (dataLen != sizeof(RoleAnnounceAckPayload) || !chainDuelManager) return;
    const RoleAnnounceAckPayload* payload = reinterpret_cast<const RoleAnnounceAckPayload*>(data);
    chainDuelManager->onRoleAnnounceAckReceived(fromMac, payload->seqId);
}

void Quickdraw::onStateLoop(Device* pdn) {
    if (chainDuelManager) chainDuelManager->sync();

    if (chainDuelManager) {
        bool loopNow = chainDuelManager->isLoop();
        if (loopNow != lastIsLoop) {
            LOG_W("CDM", "isLoop %d -> %d", (int)lastIsLoop, (int)loopNow);
            lastIsLoop = loopNow;
        }
    }

    if (!statsLogTimer.isRunning()) {
        statsLogTimer.setTimer(kStatsLogIntervalMs);
    } else if (statsLogTimer.expired()) {
        if (remoteDeviceCoordinator != nullptr && chainDuelManager != nullptr) {
            auto r = remoteDeviceCoordinator->getRetryStats();
            auto c = chainDuelManager->getRetryStats();
            unsigned long rMean = r.ackCount ? (r.ackLatencyMsSum / r.ackCount) : 0;
            unsigned long cMean = c.ackCount ? (c.ackLatencyMsSum / c.ackCount) : 0;
            // LOG_W (not LOG_I) because firmware builds with CORE_DEBUG_LEVEL=2
            // which strips info-level calls.
            LOG_W("STATS",
                "RDC s=%u r=%u ab=%u ack=%u/%lums | CDM s=%u r=%u ab=%u ack=%u/%lums",
                (unsigned)r.sends, (unsigned)r.retries, (unsigned)r.abandons,
                (unsigned)r.ackCount, rMean,
                (unsigned)c.sends, (unsigned)c.retries, (unsigned)c.abandons,
                (unsigned)c.ackCount, cMean);
        }
        statsLogTimer.setTimer(kStatsLogIntervalMs);
    }

    StateMachine::onStateLoop(pdn);
}

void Quickdraw::onChainGameEventPacket(const uint8_t* fromMac, const uint8_t* data, size_t dataLen) {
    if (dataLen != sizeof(ChainGameEventPayload)) return;
    if (!chainDuelManager || !chainDuelManager->isKnownGameEventSender(fromMac)) return;

    const ChainGameEventPayload* payload = reinterpret_cast<const ChainGameEventPayload*>(data);

    // Out-of-state events dropped silently; champion's retry machine bounds traffic cost.
    if (supporterReadyState != nullptr && currentState != nullptr
        && currentState->getStateId() == SUPPORTER_READY) {
        supporterReadyState->onChainGameEventReceived(payload->event_type, fromMac);
    }

    // ACK regardless of whether we were in SupporterReady. Not ACKing after
    // leaving the state would let the champion keep retrying a WIN/LOSS we
    // already received and can't act on. seqId=0 is the sentinel for
    // fire-and-forget events (COUNTDOWN/DRAW) and must not be ACKed.
    if (payload->seqId != 0) {
        chainDuelManager->sendGameEventAck(fromMac, payload->seqId);
    }
}

void Quickdraw::onChainGameEventAckPacket(const uint8_t* fromMac, const uint8_t* data, size_t dataLen) {
    if (dataLen != sizeof(ChainGameEventAckPayload) || !chainDuelManager) return;
    const ChainGameEventAckPayload* payload = reinterpret_cast<const ChainGameEventAckPayload*>(data);
    chainDuelManager->onChainGameEventAckReceived(fromMac, payload->seqId);
}

void Quickdraw::onChainConfirmPacket(const uint8_t* fromMac, const uint8_t* data, size_t dataLen) {
    if (dataLen != sizeof(ChainConfirmPayload)) return;
    if (!chainDuelManager) return;
    const ChainConfirmPayload* payload = reinterpret_cast<const ChainConfirmPayload*>(data);
    chainDuelManager->onConfirmReceived(fromMac, payload->originatorMac, payload->seqId);
}

void Quickdraw::onShootoutCommandPacket(const uint8_t* fromMac, const uint8_t* data, size_t dataLen) {
    if (!shootoutManager || dataLen < 2) return;
    if (data[0] > static_cast<uint8_t>(ShootoutCmd::ABORT)) return;
    ShootoutCmd cmd = static_cast<ShootoutCmd>(data[0]);
    uint8_t seqId = data[1];
    const uint8_t* payload = data + 2;
    size_t payloadLen = dataLen - 2;
    switch (cmd) {
        case ShootoutCmd::CONFIRM: {
            if (payloadLen < 6) break;
            const char* name = (payloadLen >= 6 + ShootoutManager::kNameLength)
                ? reinterpret_cast<const char*>(payload + 6) : nullptr;
            shootoutManager->onConfirmReceived(payload, name);
            break;
        }
        case ShootoutCmd::BRACKET: {
            if (payloadLen < 1) break;
            uint8_t count = payload[0];
            if (count > ShootoutManager::MAX_BRACKET_SIZE) break;
            if (payloadLen < 1 + 6 * static_cast<size_t>(count)) break;
            std::vector<std::array<uint8_t, 6>> bracket;
            bracket.reserve(count);
            for (uint8_t i = 0; i < count; i++) {
                std::array<uint8_t, 6> mac;
                memcpy(mac.data(), payload + 1 + 6 * i, 6);
                bracket.push_back(mac);
            }
            shootoutManager->onBracketReceived(bracket, seqId);
            break;
        }
        case ShootoutCmd::MATCH_START:
            if (payloadLen >= 13)
                shootoutManager->onMatchStartReceived(payload, payload + 6, payload[12], seqId);
            break;
        case ShootoutCmd::MATCH_RESULT:
            if (payloadLen >= 13)
                shootoutManager->onMatchResultReceived(payload, payload + 6, payload[12], seqId, fromMac);
            break;
        case ShootoutCmd::TOURNAMENT_END:
            if (payloadLen >= 6) shootoutManager->onTournamentEndReceived(payload, seqId);
            break;
        case ShootoutCmd::PEER_LOST:
            if (payloadLen >= 6) shootoutManager->onPeerLostReceived(payload);
            break;
        case ShootoutCmd::ABORT:
            shootoutManager->onAbortReceived(fromMac);
            break;
    }
}

void Quickdraw::onShootoutCommandAckPacket(const uint8_t* fromMac, const uint8_t* data, size_t dataLen) {
    if (!shootoutManager || dataLen < 2) return;
    if (data[0] > static_cast<uint8_t>(ShootoutCmd::ABORT)) return;
    ShootoutCmd cmd = static_cast<ShootoutCmd>(data[0]);
    uint8_t seqId = data[1];
    switch (cmd) {
        case ShootoutCmd::BRACKET:
            shootoutManager->onBracketAckReceived(fromMac, seqId);
            break;
        case ShootoutCmd::MATCH_START:
            shootoutManager->onMatchStartAckReceived(fromMac, seqId);
            break;
        case ShootoutCmd::MATCH_RESULT:
            shootoutManager->onMatchResultAckReceived(fromMac, seqId);
            break;
        case ShootoutCmd::TOURNAMENT_END:
            shootoutManager->onTournamentEndAckReceived(fromMac, seqId);
            break;
        default:
            break;
    }
}

Quickdraw::~Quickdraw() {
    // Both callbacks capture `this` and are held by objects that outlive this
    // state machine, so they must be dropped before the capture dangles.
    if (player) player->setOnRoleChanged(nullptr);
    if (remoteDeviceCoordinator) remoteDeviceCoordinator->setSelfProfileProvider(nullptr);
    player = nullptr;
    // The coordinator and the wireless manager are device-owned and outlive this
    // app, so every slot holding `this` has to be emptied here. kSymbolMatchCommand
    // is deliberately absent: its ctx is the symbol manager, which outlives
    // Quickdraw, so clearing it here would deafen a live consumer.
    remoteDeviceCoordinator->setChainChangeCallback(nullptr);
    remoteDeviceCoordinator->setPeerLostCallback(nullptr);
    remoteDeviceCoordinator = nullptr;
    for (PktType handled : {PktType::kChainGameEvent, PktType::kChainGameEventAck,
                            PktType::kChainConfirm, PktType::kRoleAnnounce,
                            PktType::kRoleAnnounceAck, PktType::kShootoutCommand,
                            PktType::kShootoutCommandAck}) {
        wirelessManager->clearEspNowPacketHandler(handled);
    }
    if (quickdrawWirelessManager) {
        quickdrawWirelessManager->clearCallbacks();
    }
    quickdrawWirelessManager = nullptr;
    delete matchManager;
    matchManager = nullptr;
    symbolWirelessManager = nullptr;
    delete chainDuelManager;
    chainDuelManager = nullptr;
    delete shootoutManager;
    shootoutManager = nullptr;
    storageManager = nullptr;
    peerComms = nullptr;
    matches.clear();
}

void Quickdraw::populateStateMap() {
    GameContext ctx;
    ctx.player = player;
    ctx.matchManager = matchManager;
    ctx.remoteDeviceCoordinator = remoteDeviceCoordinator;
    ctx.chainDuelManager = chainDuelManager;
    ctx.shootoutManager = shootoutManager;
    ctx.quickdrawWirelessManager = quickdrawWirelessManager;
    ctx.symbolWirelessManager = symbolWirelessManager;
    ctx.wirelessManager = wirelessManager;

    supporterReadyState = QuickdrawStateBuilder::build(stateMap, ctx, remoteDebugManager);
}
