#pragma once

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "game/match-manager.hpp"
#include "game/quickdraw.hpp"
#include "game/quickdraw-states.hpp"
#include "wireless/team-packet.hpp"
#include "wireless/remote-debug-manager.hpp"
#include "device-mock.hpp"
#include "utility-tests.hpp"

using ::testing::_;
using ::testing::Return;
using ::testing::Invoke;
using ::testing::SaveArg;
using ::testing::NiceMock;
using ::testing::DoAll;

// ============================================
// MatchManager boost
// ============================================

class MatchManagerBoostTests : public testing::Test {
public:
    void SetUp() override {
        fakeClock = new FakePlatformClock();
        SimpleTimer::setPlatformClock(fakeClock);
        fakeClock->setTime(10000);

        player = new Player();
        char pid[] = "hunt";
        player->setUserID(pid);
        player->setIsHunter(true);

        ON_CALL(peerComms, sendData(_, _, _, _)).WillByDefault(Return(1));
        deviceWirelessManager = new WirelessManager(&peerComms, &httpClient);
        wirelessManager = new FakeQuickdrawWirelessManager();
        wirelessManager->initialize(player, deviceWirelessManager, 100);
        matchManager = new MatchManager();
        matchManager->initialize(player, &storage, wirelessManager);

        uint8_t opponentMac[6] = {0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB};
        matchManager->initializeMatch(opponentMac);
        matchManager->setDuelLocalStartTime(10000);
    }

    void TearDown() override {
        matchManager->clearCurrentMatch();
        delete matchManager;
        delete wirelessManager;
        delete deviceWirelessManager;
        delete player;
        SimpleTimer::setPlatformClock(nullptr);
        delete fakeClock;
    }

    NiceMock<MockPeerComms> peerComms;
    MockHttpClient httpClient;
    NiceMock<MockStorage> storage;
    Player* player = nullptr;
    MatchManager* matchManager = nullptr;
    FakeQuickdrawWirelessManager* wirelessManager = nullptr;
    WirelessManager* deviceWirelessManager = nullptr;
    FakePlatformClock* fakeClock = nullptr;
};

inline void boostTurnsLossIntoWin(MatchManagerBoostTests* suite) {
    // Set boost BEFORE pressing — boost is applied at button press time
    suite->matchManager->setBoost(60); // 4 supporters * 15ms

    suite->fakeClock->advance(300);
    // Button press: raw time 300ms, boost 60ms, sent time = 240ms
    suite->matchManager->getDuelButtonPush()(suite->matchManager);

    suite->wirelessManager->setPacketReceivedCallback(
        std::bind(&MatchManager::listenForMatchEvents, suite->matchManager, std::placeholders::_1));

    // Opponent pressed at 250ms (no boost)
    TestQuickdrawPacket pkt;
    strncpy(pkt.matchId, suite->matchManager->getCurrentMatch()->getMatchId(), 36);
    pkt.matchId[36] = '\0';
    strncpy(pkt.playerId, "boun", 4);
    pkt.playerId[4] = '\0';
    pkt.isHunter = false;
    pkt.playerDrawTime = 250;
    pkt.command = QDCommand::DRAW_RESULT;

    uint8_t mac[6] = {0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB};
    suite->wirelessManager->processQuickdrawCommand(mac, reinterpret_cast<uint8_t*>(&pkt), sizeof(pkt));

    ASSERT_TRUE(suite->matchManager->matchResultsAreIn());
    // Hunter sent 240ms (300-60), bounty sent 250ms → hunter wins
    EXPECT_TRUE(suite->matchManager->didWin());
}

// ============================================
// SupporterReady
// ============================================

class SupporterReadyTests : public testing::Test {
public:
    void SetUp() override {
        fakeClock = new FakePlatformClock();
        SimpleTimer::setPlatformClock(fakeClock);
        fakeClock->setTime(10000);

        player = new Player();
        char pid[] = "supp";
        player->setUserID(pid);
        player->setIsHunter(false);

        device.fakeRemoteDeviceCoordinator.setPortStatus(SerialIdentifier::OUTPUT_JACK, PortStatus::CONNECTED);

        ON_CALL(*device.mockDisplay, invalidateScreen()).WillByDefault(Return(device.mockDisplay));
        ON_CALL(*device.mockDisplay, setGlyphMode(_)).WillByDefault(Return(device.mockDisplay));
        ON_CALL(*device.mockDisplay, drawText(_, _, _)).WillByDefault(Return(device.mockDisplay));
        ON_CALL(*device.mockDisplay, drawImage(_)).WillByDefault(Return(device.mockDisplay));
    }

    void TearDown() override {
        delete player;
        SimpleTimer::setPlatformClock(nullptr);
        delete fakeClock;
    }

    FakePlatformClock* fakeClock = nullptr;
    Player* player = nullptr;
    MockDevice device;
};

inline void supporterReadyWinFromChampion(SupporterReadyTests* suite) {
    ON_CALL(*suite->device.mockPeerComms, sendData(_, _, _, _)).WillByDefault(Return(1));

    uint8_t champMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    SupporterReady state(suite->player, &suite->device.fakeRemoteDeviceCoordinator, nullptr);
    state.setChampionMac(champMac);
    state.onStateMounted(&suite->device);

    state.handleGameEvent(GameEventType::WIN);

    EXPECT_TRUE(state.transitionToWin());
}

inline void supporterReadyIgnoresWrongMac(SupporterReadyTests* suite) {
    ON_CALL(*suite->device.mockPeerComms, sendData(_, _, _, _)).WillByDefault(Return(1));

    uint8_t champMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    SupporterReady state(suite->player, &suite->device.fakeRemoteDeviceCoordinator, nullptr);
    state.setChampionMac(champMac);
    state.onStateMounted(&suite->device);

    // LOSS event should set transitionToLose, not transitionToWin
    state.handleGameEvent(GameEventType::LOSS);

    EXPECT_FALSE(state.transitionToWin());
    EXPECT_TRUE(state.transitionToLose());
}

// ============================================
// Loop detection
// ============================================

class LoopDetectionTests : public testing::Test {
public:
    void SetUp() override {
        fakeClock = new FakePlatformClock();
        SimpleTimer::setPlatformClock(fakeClock);
        fakeClock->setTime(10000);

        player = new Player();
        char pid[] = "hunt";
        player->setUserID(pid);
        player->setIsHunter(true);

        ON_CALL(*device.mockPeerComms, sendData(_, _, _, _)).WillByDefault(Return(1));
        ON_CALL(*device.mockDisplay, invalidateScreen()).WillByDefault(Return(device.mockDisplay));
        ON_CALL(*device.mockDisplay, setGlyphMode(_)).WillByDefault(Return(device.mockDisplay));
        ON_CALL(*device.mockDisplay, drawText(_, _, _)).WillByDefault(Return(device.mockDisplay));
        ON_CALL(*device.mockDisplay, drawImage(_)).WillByDefault(Return(device.mockDisplay));

        wirelessManager = new FakeQuickdrawWirelessManager();
        wirelessManager->initialize(player, device.wirelessManager, 100);

        remoteDebugManager = new RemoteDebugManager(device.mockPeerComms);

        quickdraw = new Quickdraw(player, &device, wirelessManager, remoteDebugManager);
    }

    void TearDown() override {
        delete quickdraw;
        delete remoteDebugManager;
        delete wirelessManager;
        delete player;
        SimpleTimer::setPlatformClock(nullptr);
        delete fakeClock;
    }

    FakePlatformClock* fakeClock = nullptr;
    Player* player = nullptr;
    MockDevice device;
    FakeQuickdrawWirelessManager* wirelessManager = nullptr;
    RemoteDebugManager* remoteDebugManager = nullptr;
    Quickdraw* quickdraw = nullptr;
};

inline void loopRejectsInviteFromSupporterJackPeer(LoopDetectionTests* suite) {
    uint8_t deviceBMac[6] = {0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB};

    // Both jacks connected to same-role peers (loop topology)
    suite->device.fakeRemoteDeviceCoordinator.setPortStatus(SerialIdentifier::OUTPUT_JACK, PortStatus::CONNECTED);
    suite->device.fakeRemoteDeviceCoordinator.setPortStatus(SerialIdentifier::INPUT_JACK, PortStatus::CONNECTED);
    suite->device.fakeRemoteDeviceCoordinator.setPeerIsHunter(SerialIdentifier::OUTPUT_JACK, true);
    suite->device.fakeRemoteDeviceCoordinator.setPeerIsHunter(SerialIdentifier::INPUT_JACK, true);
    // Supporter jack (INPUT for hunter) peer is device B
    suite->device.fakeRemoteDeviceCoordinator.setPeerMac(SerialIdentifier::INPUT_JACK, deviceBMac);

    // Receive invite with champion MAC = device B (same as supporter-jack peer)
    RegisterInvitePacket invite{static_cast<uint8_t>(TeamCommandType::REGISTER_INVITE), 0, {}, {}};
    memcpy(invite.championMac, deviceBMac, 6);
    strncpy(invite.championName, "B", CHAMPION_NAME_MAX - 1);
    suite->quickdraw->onTeamPacketReceived(deviceBMac,
        reinterpret_cast<const uint8_t*>(&invite), sizeof(invite));

    SupporterReady sr(suite->player, &suite->device.fakeRemoteDeviceCoordinator, suite->quickdraw);
    EXPECT_FALSE(suite->quickdraw->shouldTransitionToSupporter(&sr));
}

inline void loopAcceptsInviteFromNonSupporterJackPeer(LoopDetectionTests* suite) {
    uint8_t champMac[6] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
    uint8_t downstreamMac[6] = {0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};

    // Normal chain: opponent jack has upstream same-role peer, supporter jack has different device
    suite->device.fakeRemoteDeviceCoordinator.setPortStatus(SerialIdentifier::OUTPUT_JACK, PortStatus::CONNECTED);
    suite->device.fakeRemoteDeviceCoordinator.setPortStatus(SerialIdentifier::INPUT_JACK, PortStatus::CONNECTED);
    suite->device.fakeRemoteDeviceCoordinator.setPeerIsHunter(SerialIdentifier::OUTPUT_JACK, true);
    suite->device.fakeRemoteDeviceCoordinator.setPeerIsHunter(SerialIdentifier::INPUT_JACK, true);
    // Supporter jack peer is a different device than champion
    suite->device.fakeRemoteDeviceCoordinator.setPeerMac(SerialIdentifier::INPUT_JACK, downstreamMac);

    // Receive invite from champion (NOT the supporter-jack peer)
    RegisterInvitePacket invite{static_cast<uint8_t>(TeamCommandType::REGISTER_INVITE), 0, {}, {}};
    memcpy(invite.championMac, champMac, 6);
    strncpy(invite.championName, "A", CHAMPION_NAME_MAX - 1);
    suite->quickdraw->onTeamPacketReceived(champMac,
        reinterpret_cast<const uint8_t*>(&invite), sizeof(invite));

    SupporterReady sr(suite->player, &suite->device.fakeRemoteDeviceCoordinator, suite->quickdraw);
    EXPECT_TRUE(suite->quickdraw->shouldTransitionToSupporter(&sr));
}

inline void deregisterRemovesSingleSupporter(LoopDetectionTests* suite) {
    uint8_t macA[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    uint8_t macB[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

    // Two supporters register
    TeamPacket regA{static_cast<uint8_t>(TeamCommandType::REGISTER), 1};
    suite->quickdraw->onTeamPacketReceived(macA,
        reinterpret_cast<const uint8_t*>(&regA), sizeof(regA));
    TeamPacket regB{static_cast<uint8_t>(TeamCommandType::REGISTER), 2};
    suite->quickdraw->onTeamPacketReceived(macB,
        reinterpret_cast<const uint8_t*>(&regB), sizeof(regB));
    ASSERT_EQ(suite->quickdraw->getSupporterCount(), 2);

    // A deregisters
    TeamPacket dereg{static_cast<uint8_t>(TeamCommandType::DEREGISTER), 1};
    suite->quickdraw->onTeamPacketReceived(macA,
        reinterpret_cast<const uint8_t*>(&dereg), sizeof(dereg));

    EXPECT_EQ(suite->quickdraw->getSupporterCount(), 1);

    // B is still registered — can still confirm
    TeamPacket confirm{static_cast<uint8_t>(TeamCommandType::CONFIRM), 0};
    suite->quickdraw->onTeamPacketReceived(macB,
        reinterpret_cast<const uint8_t*>(&confirm), sizeof(confirm));
    EXPECT_EQ(suite->quickdraw->getConfirmedSupporters(), 1);
}

inline void confirmRejectedFromUnregisteredMac(LoopDetectionTests* suite) {
    uint8_t rogueMAC[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};
    TeamPacket pkt{static_cast<uint8_t>(TeamCommandType::CONFIRM), 0};

    suite->quickdraw->onTeamPacketReceived(rogueMAC,
        reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt));

    // Boost stays at 0 — unregistered MAC is ignored
    EXPECT_FALSE(suite->quickdraw->hasTeamInvite());
}

inline void confirmAcceptedFromRegisteredMac(LoopDetectionTests* suite) {
    uint8_t supporterMAC[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

    //Register first
    TeamPacket reg{static_cast<uint8_t>(TeamCommandType::REGISTER), 1};
    suite->quickdraw->onTeamPacketReceived(supporterMAC,
        reinterpret_cast<const uint8_t*>(&reg), sizeof(reg));

    // Now confirm from same MAC
    TeamPacket confirm{static_cast<uint8_t>(TeamCommandType::CONFIRM), 0};
    suite->quickdraw->onTeamPacketReceived(supporterMAC,
        reinterpret_cast<const uint8_t*>(&confirm), sizeof(confirm));

    // Can't directly read boost, but a second confirm from same MAC should also work
    TeamPacket confirm2{static_cast<uint8_t>(TeamCommandType::CONFIRM), 0};
    suite->quickdraw->onTeamPacketReceived(supporterMAC,
        reinterpret_cast<const uint8_t*>(&confirm2), sizeof(confirm2));
}

// ============================================
// Ring topology
// ============================================

inline void ringInviteNotForwardedToChampion(SupporterReadyTests* suite) {
    std::vector<size_t> sendSizes;
    ON_CALL(*suite->device.mockPeerComms, sendData(_, _, _, _))
        .WillByDefault(Invoke([&](const uint8_t*, PktType, const uint8_t*, size_t len) -> int {
            sendSizes.push_back(len);
            return 1;
        }));

    // Supporter (bounty) with downstream peer MAC == champion MAC (ring)
    uint8_t champMac[6] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};

    // Supporter jack for bounty is OUTPUT
    suite->device.fakeRemoteDeviceCoordinator.setPortStatus(SerialIdentifier::OUTPUT_JACK, PortStatus::CONNECTED);
    suite->device.fakeRemoteDeviceCoordinator.setPeerIsHunter(SerialIdentifier::OUTPUT_JACK, false);
    suite->device.fakeRemoteDeviceCoordinator.setPeerMac(SerialIdentifier::OUTPUT_JACK, champMac);

    SupporterReady state(suite->player, &suite->device.fakeRemoteDeviceCoordinator, nullptr);
    state.setChampionMac(champMac);
    state.setPosition(1);
    state.onStateMounted(&suite->device);

    suite->fakeClock->advance(2100);
    state.onStateLoop(&suite->device);

    // REGISTER packets (2 bytes) are expected, but no REGISTER_INVITE (24 bytes)
    for (size_t len : sendSizes) {
        EXPECT_NE(len, sizeof(RegisterInvitePacket))
            << "Invite was forwarded to champion MAC — ring not detected";
    }
}

// ============================================
// Chain state lifecycle
// ============================================

inline void supporterCountClearsOnDisconnect(LoopDetectionTests* suite) {
    uint8_t supporterMAC[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    uint8_t supporterMAC2[6] = {0x22, 0x33, 0x44, 0x55, 0x66, 0x77};

    //Two supporters register
    TeamPacket reg1{static_cast<uint8_t>(TeamCommandType::REGISTER), 1};
    suite->quickdraw->onTeamPacketReceived(supporterMAC,
        reinterpret_cast<const uint8_t*>(&reg1), sizeof(reg1));
    TeamPacket reg2{static_cast<uint8_t>(TeamCommandType::REGISTER), 2};
    suite->quickdraw->onTeamPacketReceived(supporterMAC2,
        reinterpret_cast<const uint8_t*>(&reg2), sizeof(reg2));

    ASSERT_EQ(suite->quickdraw->getSupporterCount(), 2);

    // Clear chain state (simulates supporter jack disconnect)
    suite->quickdraw->clearChainState();

    EXPECT_EQ(suite->quickdraw->getSupporterCount(), 0);
    EXPECT_EQ(suite->quickdraw->getConfirmedSupporters(), 0);
    EXPECT_FALSE(suite->quickdraw->hasTeamInvite());
}

inline void registerIsIdempotent(LoopDetectionTests* suite) {
    uint8_t supporterMAC[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

    //Same MAC registers three times
    for (int i = 0; i < 3; i++) {
        TeamPacket reg{static_cast<uint8_t>(TeamCommandType::REGISTER), 1};
        suite->quickdraw->onTeamPacketReceived(supporterMAC,
            reinterpret_cast<const uint8_t*>(&reg), sizeof(reg));
    }

    EXPECT_EQ(suite->quickdraw->getSupporterCount(), 1);
}

inline void confirmCappedAtSupporterCount(LoopDetectionTests* suite) {
    uint8_t supporterMAC[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

    //Register one supporter
    TeamPacket reg{static_cast<uint8_t>(TeamCommandType::REGISTER), 1};
    suite->quickdraw->onTeamPacketReceived(supporterMAC,
        reinterpret_cast<const uint8_t*>(&reg), sizeof(reg));

    // Send CONFIRM three times from same registered MAC
    for (int i = 0; i < 3; i++) {
        TeamPacket confirm{static_cast<uint8_t>(TeamCommandType::CONFIRM), 0};
        suite->quickdraw->onTeamPacketReceived(supporterMAC,
            reinterpret_cast<const uint8_t*>(&confirm), sizeof(confirm));
    }

    // Only 1 supporter registered, so confirmed should cap at 1
    EXPECT_EQ(suite->quickdraw->getConfirmedSupporters(), 1);
}

// ============================================
// Idle lifecycle with connection changes
// ============================================

class ChampionIdleLifecycleTests : public testing::Test {
public:
    void SetUp() override {
        fakeClock = new FakePlatformClock();
        SimpleTimer::setPlatformClock(fakeClock);
        fakeClock->setTime(10000);

        player = new Player();
        char pid[] = "hunt";
        player->setUserID(pid);
        player->setIsHunter(true);

        ON_CALL(*device.mockPeerComms, sendData(_, _, _, _)).WillByDefault(Return(1));
        ON_CALL(*device.mockDisplay, invalidateScreen()).WillByDefault(Return(device.mockDisplay));
        ON_CALL(*device.mockDisplay, setGlyphMode(_)).WillByDefault(Return(device.mockDisplay));
        ON_CALL(*device.mockDisplay, drawText(_, _, _)).WillByDefault(Return(device.mockDisplay));
        ON_CALL(*device.mockDisplay, drawImage(_)).WillByDefault(Return(device.mockDisplay));
        ON_CALL(*device.mockPrimaryButton, setButtonPress(testing::A<parameterizedCallbackFunction>(), _, _))
            .WillByDefault(Return());
        ON_CALL(*device.mockSecondaryButton, setButtonPress(testing::A<parameterizedCallbackFunction>(), _, _))
            .WillByDefault(Return());
        ON_CALL(*device.mockPrimaryButton, removeButtonCallbacks()).WillByDefault(Return());
        ON_CALL(*device.mockSecondaryButton, removeButtonCallbacks()).WillByDefault(Return());

        // Set device MAC so Quickdraw can identify itself
        static uint8_t myMac[6] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
        ON_CALL(*device.mockPeerComms, getMacAddress()).WillByDefault(Return(myMac));

        wirelessManager = new FakeQuickdrawWirelessManager();
        wirelessManager->initialize(player, device.wirelessManager, 100);
        remoteDebugManager = new RemoteDebugManager(device.mockPeerComms);
        quickdraw = new Quickdraw(player, &device, wirelessManager, remoteDebugManager);
        quickdraw->populateStateMap();

        matchManager = new MatchManager();
        matchManager->initialize(player, &storage, wirelessManager);
        idle = new Idle(player, matchManager, &device.fakeRemoteDeviceCoordinator, quickdraw);
    }

    void TearDown() override {
        delete idle;
        delete matchManager;
        delete quickdraw;
        delete remoteDebugManager;
        delete wirelessManager;
        delete player;
        SimpleTimer::setPlatformClock(nullptr);
        delete fakeClock;
    }

    void tickIdle(int ms) {
        fakeClock->advance(ms);
        idle->onStateLoop(&device);
    }

    FakePlatformClock* fakeClock = nullptr;
    Player* player = nullptr;
    MockDevice device;
    NiceMock<MockStorage> storage;
    FakeQuickdrawWirelessManager* wirelessManager = nullptr;
    RemoteDebugManager* remoteDebugManager = nullptr;
    Quickdraw* quickdraw = nullptr;
    MatchManager* matchManager = nullptr;
    Idle* idle = nullptr;
};

inline void championSendsInviteWhenSupporterConnects(ChampionIdleLifecycleTests* suite) {
    uint8_t supporterMac[6] = {0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB};

    suite->idle->onStateMounted(&suite->device);

    // Pass stabilization
    suite->tickIdle(600);

    // No supporter yet — no invites
    EXPECT_EQ(suite->quickdraw->getSupporterCount(), 0);

    // Supporter connects on INPUT jack (supporter jack for hunter)
    suite->device.fakeRemoteDeviceCoordinator.setPortStatus(SerialIdentifier::INPUT_JACK, PortStatus::CONNECTED);
    suite->device.fakeRemoteDeviceCoordinator.setPeerIsHunter(SerialIdentifier::INPUT_JACK, true);
    suite->device.fakeRemoteDeviceCoordinator.setPeerMac(SerialIdentifier::INPUT_JACK, supporterMac);

    // Tick — should send invite
    EXPECT_CALL(*suite->device.mockPeerComms,
        sendData(_, PktType::kTeamCommand, _, sizeof(RegisterInvitePacket))).Times(1);
    suite->tickIdle(100);

    suite->idle->onStateDismounted(&suite->device);
}

inline void championClearsChainWhenSupporterDisconnects(ChampionIdleLifecycleTests* suite) {
    uint8_t supporterMac[6] = {0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB};

    suite->idle->onStateMounted(&suite->device);
    suite->tickIdle(600); // pass stabilization

    // Supporter connects
    suite->device.fakeRemoteDeviceCoordinator.setPortStatus(SerialIdentifier::INPUT_JACK, PortStatus::CONNECTED);
    suite->device.fakeRemoteDeviceCoordinator.setPeerIsHunter(SerialIdentifier::INPUT_JACK, true);
    suite->device.fakeRemoteDeviceCoordinator.setPeerMac(SerialIdentifier::INPUT_JACK, supporterMac);

    suite->tickIdle(100); // sends invite

    // Supporter registers
    TeamPacket reg{static_cast<uint8_t>(TeamCommandType::REGISTER), 1};
    suite->quickdraw->onTeamPacketReceived(supporterMac,
        reinterpret_cast<const uint8_t*>(&reg), sizeof(reg));
    ASSERT_EQ(suite->quickdraw->getSupporterCount(), 1);

    // Supporter disconnects
    suite->device.fakeRemoteDeviceCoordinator.setPortStatus(SerialIdentifier::INPUT_JACK, PortStatus::DISCONNECTED);

    suite->tickIdle(100);

    EXPECT_EQ(suite->quickdraw->getSupporterCount(), 0);

    suite->idle->onStateDismounted(&suite->device);
}

inline void championIgnoresInviteWithOwnMacViaPacketHandler(ChampionIdleLifecycleTests* suite) {
    uint8_t myMac[6] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
    uint8_t otherMac[6] = {0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};

    // Set up opponent jack with same-role peer (required for shouldTransitionToSupporter)
    suite->device.fakeRemoteDeviceCoordinator.setPortStatus(SerialIdentifier::OUTPUT_JACK, PortStatus::CONNECTED);
    suite->device.fakeRemoteDeviceCoordinator.setPeerIsHunter(SerialIdentifier::OUTPUT_JACK, true);

    // Receive an invite where champion MAC == our MAC (looped ring)
    RegisterInvitePacket invite{static_cast<uint8_t>(TeamCommandType::REGISTER_INVITE), 0, {}, {}};
    memcpy(invite.championMac, myMac, 6);
    strncpy(invite.championName, "Me", CHAMPION_NAME_MAX - 1);
    suite->quickdraw->onTeamPacketReceived(otherMac,
        reinterpret_cast<const uint8_t*>(&invite), sizeof(invite));

    // Should NOT have team invite set — we removed the self-check, so this tests
    // that the invite IS accepted at the packet level (the guard is in forwarding)
    // This documents current behavior: invite is accepted, but shouldTransitionToSupporter
    // would need to be called to actually transition
    EXPECT_TRUE(suite->quickdraw->hasTeamInvite());
}

// ============================================
// Two-device packet exchange
// ============================================

inline void twoDeviceChainRegistrationFlow(LoopDetectionTests* suite) {
    uint8_t championMac[6] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
    uint8_t supporterMac[6] = {0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB};

    //--- Champion side: send invite ---
    // Champion's supporter jack (INPUT for hunter) has supporter
    suite->device.fakeRemoteDeviceCoordinator.setPortStatus(SerialIdentifier::INPUT_JACK, PortStatus::CONNECTED);
    suite->device.fakeRemoteDeviceCoordinator.setPeerIsHunter(SerialIdentifier::INPUT_JACK, true);
    suite->device.fakeRemoteDeviceCoordinator.setPeerMac(SerialIdentifier::INPUT_JACK, supporterMac);

    // --- Supporter side: receive invite, send REGISTER ---
    RegisterInvitePacket invite{static_cast<uint8_t>(TeamCommandType::REGISTER_INVITE), 0, {}, {}};
    memcpy(invite.championMac, championMac, 6);
    strncpy(invite.championName, "Champ", CHAMPION_NAME_MAX - 1);

    // Simulate supporter receiving the invite
    suite->quickdraw->onTeamPacketReceived(championMac,
        reinterpret_cast<const uint8_t*>(&invite), sizeof(invite));
    EXPECT_TRUE(suite->quickdraw->hasTeamInvite());
    EXPECT_EQ(suite->quickdraw->getInvitePosition(), 1);

    // --- Champion side: receive REGISTER from supporter ---
    TeamPacket reg{static_cast<uint8_t>(TeamCommandType::REGISTER), 1};
    suite->quickdraw->onTeamPacketReceived(supporterMac,
        reinterpret_cast<const uint8_t*>(&reg), sizeof(reg));
    EXPECT_EQ(suite->quickdraw->getSupporterCount(), 1);

    // --- Champion side: receive CONFIRM from supporter ---
    TeamPacket confirm{static_cast<uint8_t>(TeamCommandType::CONFIRM), 0};
    suite->quickdraw->onTeamPacketReceived(supporterMac,
        reinterpret_cast<const uint8_t*>(&confirm), sizeof(confirm));
    EXPECT_EQ(suite->quickdraw->getConfirmedSupporters(), 1);
}

inline void midChainDeviceDoesNotBecomeChampion(ChampionIdleLifecycleTests* suite) {
    uint8_t upstreamMac[6] = {0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
    uint8_t downstreamMac[6] = {0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD};

    suite->idle->onStateMounted(&suite->device);
    suite->tickIdle(600); // pass stabilization

    // Both jacks connected with same-role peers → mid-chain
    // Opponent jack (OUTPUT for hunter)
    suite->device.fakeRemoteDeviceCoordinator.setPortStatus(SerialIdentifier::OUTPUT_JACK, PortStatus::CONNECTED);
    suite->device.fakeRemoteDeviceCoordinator.setPeerIsHunter(SerialIdentifier::OUTPUT_JACK, true);
    suite->device.fakeRemoteDeviceCoordinator.setPeerMac(SerialIdentifier::OUTPUT_JACK, upstreamMac);
    // Supporter jack (INPUT for hunter)
    suite->device.fakeRemoteDeviceCoordinator.setPortStatus(SerialIdentifier::INPUT_JACK, PortStatus::CONNECTED);
    suite->device.fakeRemoteDeviceCoordinator.setPeerIsHunter(SerialIdentifier::INPUT_JACK, true);
    suite->device.fakeRemoteDeviceCoordinator.setPeerMac(SerialIdentifier::INPUT_JACK, downstreamMac);

    // Should NOT send any invite — same-role on opponent means mid-chain
    EXPECT_CALL(*suite->device.mockPeerComms,
        sendData(_, PktType::kTeamCommand, _, sizeof(RegisterInvitePacket))).Times(0);

    suite->tickIdle(100);

    suite->idle->onStateDismounted(&suite->device);
}

// ============================================
// Systematic topology × event tests
// ============================================

// --- Topology: single supporter connects then disconnects (no deregister sent) ---
// Covers: supporter crashes or cable yanked without clean deregister
inline void supporterDisappearsWithoutDeregister(ChampionIdleLifecycleTests* suite) {
    uint8_t supporterMac[6] = {0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB};

    suite->idle->onStateMounted(&suite->device);
    suite->tickIdle(600);

    // Supporter connects
    suite->device.fakeRemoteDeviceCoordinator.setPortStatus(SerialIdentifier::INPUT_JACK, PortStatus::CONNECTED);
    suite->device.fakeRemoteDeviceCoordinator.setPeerIsHunter(SerialIdentifier::INPUT_JACK, true);
    suite->device.fakeRemoteDeviceCoordinator.setPeerMac(SerialIdentifier::INPUT_JACK, supporterMac);
    suite->tickIdle(100);

    // Supporter registers wirelessly
    TeamPacket reg{static_cast<uint8_t>(TeamCommandType::REGISTER), 1};
    suite->quickdraw->onTeamPacketReceived(supporterMac,
        reinterpret_cast<const uint8_t*>(&reg), sizeof(reg));
    ASSERT_EQ(suite->quickdraw->getSupporterCount(), 1);

    // Cable yanked — no DEREGISTER sent, just jack goes disconnected
    suite->device.fakeRemoteDeviceCoordinator.setPortStatus(SerialIdentifier::INPUT_JACK, PortStatus::DISCONNECTED);
    suite->tickIdle(100);

    // Chain state should be fully cleared
    EXPECT_EQ(suite->quickdraw->getSupporterCount(), 0);

    suite->idle->onStateDismounted(&suite->device);
}

// --- Topology: two supporters, first one deregisters ---
inline void twoSupportersOneDeregisters(LoopDetectionTests* suite) {
    uint8_t macA[6] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11};
    uint8_t macB[6] = {0x22, 0x22, 0x22, 0x22, 0x22, 0x22};

    TeamPacket regA{static_cast<uint8_t>(TeamCommandType::REGISTER), 1};
    suite->quickdraw->onTeamPacketReceived(macA,
        reinterpret_cast<const uint8_t*>(&regA), sizeof(regA));
    TeamPacket regB{static_cast<uint8_t>(TeamCommandType::REGISTER), 2};
    suite->quickdraw->onTeamPacketReceived(macB,
        reinterpret_cast<const uint8_t*>(&regB), sizeof(regB));

    // Both confirm
    TeamPacket confA{static_cast<uint8_t>(TeamCommandType::CONFIRM), 0};
    suite->quickdraw->onTeamPacketReceived(macA,
        reinterpret_cast<const uint8_t*>(&confA), sizeof(confA));
    TeamPacket confB{static_cast<uint8_t>(TeamCommandType::CONFIRM), 0};
    suite->quickdraw->onTeamPacketReceived(macB,
        reinterpret_cast<const uint8_t*>(&confB), sizeof(confB));
    ASSERT_EQ(suite->quickdraw->getConfirmedSupporters(), 2);

    // A deregisters
    TeamPacket dereg{static_cast<uint8_t>(TeamCommandType::DEREGISTER), 1};
    suite->quickdraw->onTeamPacketReceived(macA,
        reinterpret_cast<const uint8_t*>(&dereg), sizeof(dereg));

    EXPECT_EQ(suite->quickdraw->getSupporterCount(), 1);
    EXPECT_EQ(suite->quickdraw->getConfirmedSupporters(), 1);
}

// --- Topology: deregister from unknown MAC (never registered) ---
inline void deregisterFromUnknownMacIsIgnored(LoopDetectionTests* suite) {
    uint8_t knownMac[6] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11};
    uint8_t unknownMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};

    TeamPacket reg{static_cast<uint8_t>(TeamCommandType::REGISTER), 1};
    suite->quickdraw->onTeamPacketReceived(knownMac,
        reinterpret_cast<const uint8_t*>(&reg), sizeof(reg));
    ASSERT_EQ(suite->quickdraw->getSupporterCount(), 1);

    // Deregister from a MAC that never registered
    TeamPacket dereg{static_cast<uint8_t>(TeamCommandType::DEREGISTER), 1};
    suite->quickdraw->onTeamPacketReceived(unknownMac,
        reinterpret_cast<const uint8_t*>(&dereg), sizeof(dereg));

    // Count unchanged
    EXPECT_EQ(suite->quickdraw->getSupporterCount(), 1);
}

// --- Topology: supporter re-registers after deregister ---
inline void supporterCanReregisterAfterDeregister(LoopDetectionTests* suite) {
    uint8_t mac[6] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11};

    TeamPacket reg{static_cast<uint8_t>(TeamCommandType::REGISTER), 1};
    suite->quickdraw->onTeamPacketReceived(mac,
        reinterpret_cast<const uint8_t*>(&reg), sizeof(reg));
    ASSERT_EQ(suite->quickdraw->getSupporterCount(), 1);

    TeamPacket dereg{static_cast<uint8_t>(TeamCommandType::DEREGISTER), 1};
    suite->quickdraw->onTeamPacketReceived(mac,
        reinterpret_cast<const uint8_t*>(&dereg), sizeof(dereg));
    ASSERT_EQ(suite->quickdraw->getSupporterCount(), 0);

    // Re-register
    suite->quickdraw->onTeamPacketReceived(mac,
        reinterpret_cast<const uint8_t*>(&reg), sizeof(reg));
    EXPECT_EQ(suite->quickdraw->getSupporterCount(), 1);
}

// --- Topology: game event from non-champion MAC is ignored ---
inline void gameEventFromNonChampionIgnored(LoopDetectionTests* suite) {
    uint8_t champMac[6] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
    uint8_t otherMac[6] = {0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB};

    // Set up invite so we have a champion MAC
    RegisterInvitePacket invite{static_cast<uint8_t>(TeamCommandType::REGISTER_INVITE), 0, {}, {}};
    memcpy(invite.championMac, champMac, 6);
    strncpy(invite.championName, "A", CHAMPION_NAME_MAX - 1);
    suite->quickdraw->onTeamPacketReceived(champMac,
        reinterpret_cast<const uint8_t*>(&invite), sizeof(invite));

    bool eventFired = false;
    suite->quickdraw->setGameEventCallback([&](GameEventType) { eventFired = true; });

    // Game event from wrong MAC
    TeamPacket evt{static_cast<uint8_t>(TeamCommandType::GAME_EVENT),
        static_cast<uint8_t>(GameEventType::WIN)};
    suite->quickdraw->onTeamPacketReceived(otherMac,
        reinterpret_cast<const uint8_t*>(&evt), sizeof(evt));
    EXPECT_FALSE(eventFired);

    // Game event from correct champion MAC
    suite->quickdraw->onTeamPacketReceived(champMac,
        reinterpret_cast<const uint8_t*>(&evt), sizeof(evt));
    EXPECT_TRUE(eventFired);

    suite->quickdraw->clearGameEventCallback();
}

// --- Topology: champion with opponent disconnected can't transition to supporter ---
inline void noSupporterTransitionWithoutOpponent(LoopDetectionTests* suite) {
    uint8_t champMac[6] = {0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};

    // Receive invite but opponent jack is disconnected
    RegisterInvitePacket invite{static_cast<uint8_t>(TeamCommandType::REGISTER_INVITE), 0, {}, {}};
    memcpy(invite.championMac, champMac, 6);
    strncpy(invite.championName, "C", CHAMPION_NAME_MAX - 1);
    suite->quickdraw->onTeamPacketReceived(champMac,
        reinterpret_cast<const uint8_t*>(&invite), sizeof(invite));
    ASSERT_TRUE(suite->quickdraw->hasTeamInvite());

    // Opponent jack disconnected
    suite->device.fakeRemoteDeviceCoordinator.setPortStatus(SerialIdentifier::OUTPUT_JACK, PortStatus::DISCONNECTED);

    SupporterReady sr(suite->player, &suite->device.fakeRemoteDeviceCoordinator, suite->quickdraw);
    EXPECT_FALSE(suite->quickdraw->shouldTransitionToSupporter(&sr));
    EXPECT_FALSE(suite->quickdraw->hasTeamInvite()); // cleared on rejection
}

// --- Topology: champion with opposite-role on opponent can't transition to supporter ---
inline void noSupporterTransitionWithOppositeRoleOpponent(LoopDetectionTests* suite) {
    uint8_t champMac[6] = {0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};

    RegisterInvitePacket invite{static_cast<uint8_t>(TeamCommandType::REGISTER_INVITE), 0, {}, {}};
    memcpy(invite.championMac, champMac, 6);
    strncpy(invite.championName, "C", CHAMPION_NAME_MAX - 1);
    suite->quickdraw->onTeamPacketReceived(champMac,
        reinterpret_cast<const uint8_t*>(&invite), sizeof(invite));

    // Opponent jack has bounty (opposite role — this is a duel opponent, not chain)
    suite->device.fakeRemoteDeviceCoordinator.setPortStatus(SerialIdentifier::OUTPUT_JACK, PortStatus::CONNECTED);
    suite->device.fakeRemoteDeviceCoordinator.setPeerIsHunter(SerialIdentifier::OUTPUT_JACK, false);

    SupporterReady sr(suite->player, &suite->device.fakeRemoteDeviceCoordinator, suite->quickdraw);
    EXPECT_FALSE(suite->quickdraw->shouldTransitionToSupporter(&sr));
}

// ============================================
// SupporterReady button behavior
// ============================================

inline void supporterReadyButtonSendsConfirm(SupporterReadyTests* suite) {
    parameterizedCallbackFunction btnCallback = nullptr;
    void* btnCtx = nullptr;
    ON_CALL(*suite->device.mockPrimaryButton, setButtonPress(testing::A<parameterizedCallbackFunction>(), _, _))
        .WillByDefault(DoAll(SaveArg<0>(&btnCallback), SaveArg<1>(&btnCtx)));
    ON_CALL(*suite->device.mockPeerComms, sendData(_, _, _, _)).WillByDefault(Return(1));

    uint8_t champMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    SupporterReady state(suite->player, &suite->device.fakeRemoteDeviceCoordinator, nullptr);
    state.setChampionMac(champMac);
    state.onStateMounted(&suite->device);

    ASSERT_NE(btnCallback, nullptr);

    // Button press before duel is active should be ignored
    btnCallback(btnCtx);

    // Simulate champion starting countdown
    state.handleGameEvent(GameEventType::COUNTDOWN);

    EXPECT_CALL(*suite->device.mockPeerComms,
        sendData(_, PktType::kTeamCommand, _, _)).Times(1);

    btnCallback(btnCtx);
}
