#pragma once

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "device-mock.hpp"
#include "utility-tests.hpp"
#include "device/mac-types.hpp"
#include "device/peer-graph-codec.hpp"
#include "device/device-type.hpp"
#include "device/remote-device-coordinator.hpp"
#include "game/chain-manager.hpp"
#include "game/player.hpp"

#include <algorithm>

using ::testing::Return;
using ::testing::_;
using ::testing::NiceMock;

class ChainManagerTests : public testing::Test {
public:
    void SetUp() override {
        fakeClock = new FakePlatformClock();
        SimpleTimer::setPlatformClock(fakeClock);
        fakeClock->setTime(1000);

        ON_CALL(*device.mockPeerComms, sendData(_, _, _, _)).WillByDefault(Return(1));
        ON_CALL(*device.mockPeerComms, addEspNowPeer(_)).WillByDefault(Return(0));
        ON_CALL(*device.mockPeerComms, removeEspNowPeer(_)).WillByDefault(Return(0));
        ON_CALL(*device.mockPeerComms, getMacAddress()).WillByDefault(Return(localMac));
        ON_CALL(*device.mockPeerComms, getPeerCommsState()).WillByDefault(Return(PeerCommsState::CONNECTED));
        ON_CALL(*device.mockPeerComms, setPacketHandler(testing::Eq(PktType::kHandshakeCommand), _, _))
            .WillByDefault(testing::DoAll(
                testing::SaveArg<1>(&capturedHandler),
                testing::SaveArg<2>(&capturedCtx)));
        rdc.initialize(device.wirelessManager, device.serialManager, &device);
        // Single-device tests have no partner emitting PROBEs, so production's
        // 2s silent-link threshold trips during multi-second time advances
        // (e.g. primeRosterStable's 3.3s window, or onChainStateChangedClearsOnDrain's
        // 11s drain test). Push the threshold past anything any test cares to wait.
        rdc.setJackDeadSilentLinkMsForTest(60000);
    }

    void TearDown() override {
        SimpleTimer::setPlatformClock(nullptr);
        delete fakeClock;
    }

    // Deliver a HELLO frame onto the local jack as if a partner had just
    // transmitted it over the serial cable. RDC parses it and sets macPeer
    // for the local jack. Mirrors production exactly.
    void deliverHello(SerialIdentifier localJack, const uint8_t* peerMac,
                      uint8_t deviceType = static_cast<uint8_t>(DeviceType::PDN)) {
        net::Mac mac;
        std::copy_n(peerMac, 6, mac.begin());
        auto frame = peer_graph::encodeHello(mac, deviceType);
        auto& serial = localJack == SerialIdentifier::INPUT_JACK
            ? device.inputJackSerial : device.outputJackSerial;
        ASSERT_NE(serial.bytesCallback, nullptr);
        serial.bytesCallback(frame.data(), frame.size());
    }

    void connectInputPort() {
        deliverHello(SerialIdentifier::INPUT_JACK, opponentMac);
        rdc.sync(&device);
    }

    void connectOutputPort() {
        deliverHello(SerialIdentifier::OUTPUT_JACK, opponentMac);
        rdc.sync(&device);
    }

    // Set up the physical hunter-champion topology (direct peers only — no
    // role state). Tests that need roles applied should call
    // applyHunterChampionRoles(cdm) afterward.
    //   OUTPUT (opponent jack) → bounty peer (opponentMac)
    //   INPUT  (supporter jack) → hunter peer (supporterMac)
    void setupHunterChampion() {
        player.setIsHunter(true);
        deliverHello(SerialIdentifier::OUTPUT_JACK, opponentMac);
        deliverHello(SerialIdentifier::INPUT_JACK, supporterMac);
        rdc.sync(&device);
    }

    void applyHunterChampionRoles(ChainManager& cdm) {
        cdm.setPeerRole(SerialIdentifier::OUTPUT_JACK, false);  // bounty opponent
        cdm.setPeerRole(SerialIdentifier::INPUT_JACK, true);    // hunter supporter
    }

    // Set up a hunter-supporter topology: same-role hunter on OUTPUT_JACK only.
    // OUTPUT_JACK is the opponent jack for hunters, so a same-role peer there
    // makes this device a supporter. No INPUT_JACK peer (no downstream supporters).
    void setupHunterSupporter() {
        player.setIsHunter(true);
        deliverHello(SerialIdentifier::OUTPUT_JACK, opponentMac);
        rdc.sync(&device);
    }

    void applyHunterSupporterRoles(ChainManager& cdm) {
        cdm.setPeerRole(SerialIdentifier::OUTPUT_JACK, true);   // same-role hunter on opponent jack
    }

    // Drive the real RDC's 1Hz stability counter to >=2 so isTopologyStable()
    // returns true. Tests that exercise onConfirmReceived need this: the
    // stability gate drops confirms while the counter is still settling.
    void primeRosterStable() {
        for (int i = 0; i < 3; ++i) {
            fakeClock->advance(1100);
            rdc.sync(&device);
        }
    }

    MockDevice device;
    RemoteDeviceCoordinator rdc;
    FakePlatformClock* fakeClock;
    Player player;

    PeerCommsInterface::PacketCallback capturedHandler = nullptr;
    void* capturedCtx = nullptr;
    uint8_t localMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    uint8_t opponentMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    uint8_t supporterMac[6] = {0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11};
};

// Stubs the three roster-query methods ChainManager derives game state from
// (getChainMembers / isInLoop / isTopologyStable), so a test can shape topology
// directly instead of driving the real PROBE/ADD machinery. Used by the
// coord-election tests and the loop-gated canInitiateMatch test.
class FakeRosterRDC : public RemoteDeviceCoordinator {
public:
    std::vector<net::Mac> members;
    bool inLoop = false;
    bool stable = true;
    size_t chainBehind = 0;
    std::vector<net::Mac> getChainMembers() const override { return members; }
    bool isInLoop() const override { return inLoop; }
    bool isTopologyStable() const override { return stable; }
    size_t countChainBehind(SerialIdentifier) const override { return chainBehind; }
};

// getChainLength reports the supporter-side depth for a champion and zero for
// anyone else: the posse belongs to the device leading it, not to a mid-chain
// supporter that happens to have its own downstream peers.
inline void cdmGetChainLengthChampionOnly(ChainManagerTests* suite) {
    FakeRosterRDC fakeRdc;
    fakeRdc.stable = true;
    fakeRdc.inLoop = false;
    fakeRdc.chainBehind = 3;
    suite->player.setIsHunter(true);

    ChainManager champ(&suite->player, suite->device.wirelessManager, &fakeRdc);
    champ.setPeerRole(SerialIdentifier::OUTPUT_JACK, false);  // bounty opponent → champion
    ASSERT_TRUE(champ.isChampion());
    EXPECT_EQ(champ.getChainLength(), 3u);

    ChainManager supporter(&suite->player, suite->device.wirelessManager, &fakeRdc);
    supporter.setPeerRole(SerialIdentifier::OUTPUT_JACK, true);  // same-role → supporter
    ASSERT_FALSE(supporter.isChampion());
    EXPECT_EQ(supporter.getChainLength(), 0u);
}

// Role derivation: champion topology produces correct is* / canInitiate / peers results.
// Without peers everything returns false/empty; with a full champion setup everything flips.
inline void cdmRoleDerivationWithChampionTopology(ChainManagerTests* suite) {
    suite->player.setIsHunter(true);
    ChainManager cdm(&suite->player, suite->device.wirelessManager, &suite->rdc);

    // No peers: default-champion (isSupporter is false, no opponent peer).
    EXPECT_TRUE(cdm.isChampion());
    EXPECT_FALSE(cdm.isSupporter());
    EXPECT_FALSE(cdm.canInitiateMatch());
    EXPECT_TRUE(cdm.getSupporterChainPeers().empty());

    suite->setupHunterChampion();
    // canInitiateMatch acts only on a stable topology, so drive the stability
    // counter before asserting it can init.
    suite->primeRosterStable();
    ChainManager cdm2(&suite->player, suite->device.wirelessManager, &suite->rdc);
    suite->applyHunterChampionRoles(cdm2);

    EXPECT_TRUE(cdm2.isChampion());
    EXPECT_FALSE(cdm2.isSupporter());
    EXPECT_TRUE(cdm2.canInitiateMatch());
    EXPECT_FALSE(cdm2.getSupporterChainPeers().empty());
}

// Bounties never initiate matches regardless of topology
inline void cdmCanInitiateMatchFalseForBounty(ChainManagerTests* suite) {
    suite->player.setIsHunter(false);
    ChainManager cdm(&suite->player, suite->device.wirelessManager, &suite->rdc);
    EXPECT_FALSE(cdm.canInitiateMatch());
}

// Loop detected: even a hunter with a bounty on their opponent jack must NOT
// initiate a duel — the Idle → ShootoutProposal path owns the next transition.
// canInitiateMatch keys off isInLoop directly (not the coordinator flag): in a
// ring only one member is coordinator, but no member may start a 1v1.
inline void cdmCanInitiateMatchFalseWhenInLoop(ChainManagerTests* suite) {
    FakeRosterRDC fakeRdc;
    fakeRdc.stable = true;
    fakeRdc.inLoop = false;
    suite->player.setIsHunter(true);
    ChainManager cdm(&suite->player, suite->device.wirelessManager, &fakeRdc);
    // Bounty on the opponent jack (OUTPUT for a hunter): a valid duel pairing.
    cdm.setPeerRole(SerialIdentifier::OUTPUT_JACK, false);

    ASSERT_TRUE(cdm.canInitiateMatch())
        << "precondition: stable, not in loop, hunter with opposite-role opponent";

    fakeRdc.inLoop = true;
    EXPECT_FALSE(cdm.canInitiateMatch())
        << "in a loop, no member may start a 1v1 — shootout owns the transition";
}

// Confirm lifecycle: starts at 0, increments per unique MAC, dedup blocks repeats, clear resets
inline void cdmConfirmLifecycle(ChainManagerTests* suite) {
    suite->setupHunterChampion();
    suite->primeRosterStable();
    ChainManager cdm(&suite->player, suite->device.wirelessManager, &suite->rdc);
    suite->applyHunterChampionRoles(cdm);
    ASSERT_TRUE(cdm.isChampion());

    EXPECT_EQ(cdm.getBoostMs(), 0u);

    // Signature: (fromMac, originatorMac, seqId).
    // For 2-device case, fromMac == originatorMac (direct delivery, no forwarder).
    cdm.onConfirmReceived(suite->supporterMac, suite->supporterMac, 1);
    EXPECT_EQ(cdm.getBoostMs(), 15u);
    EXPECT_EQ(cdm.getConfirmedSupporterCount(), 1u);

    // Same originator, different seqId → still same MAC, still deduped at champion.
    cdm.onConfirmReceived(suite->supporterMac, suite->supporterMac, 2);
    EXPECT_EQ(cdm.getConfirmedSupporterCount(), 1u);

    cdm.clearSupporterConfirms();
    EXPECT_EQ(cdm.getBoostMs(), 0u);
    EXPECT_EQ(cdm.getConfirmedSupporterCount(), 0u);
}

// 10. onChainStateChanged clears confirms when chain drains to 0
inline void cdmOnChainStateChangedClearsOnDrain(ChainManagerTests* suite) {
    suite->setupHunterChampion();
    suite->primeRosterStable();
    ChainManager cdm(&suite->player, suite->device.wirelessManager, &suite->rdc);
    suite->applyHunterChampionRoles(cdm);
    ASSERT_TRUE(cdm.isChampion());

    cdm.onConfirmReceived(suite->supporterMac, suite->supporterMac, 1);
    ASSERT_EQ(cdm.getBoostMs(), 15u);

    // Supporter-jack peer still present: the confirmed supporter is retained.
    cdm.onChainStateChanged();
    EXPECT_EQ(cdm.getBoostMs(), 15u);

    // This test specifically exercises the silent-link jack-dead drain path.
    // The fixture set silent-link to 60s to survive primeRosterStable's 3.3s
    // window in tests that don't care about jack-dead. Restore it to 2s here
    // so the 11s advance below triggers surrender.
    suite->rdc.setJackDeadSilentLinkMsForTest(2000);

    // Disconnect via the silent-link jack-dead path (>2s of no valid frames
    // on a CONNECTED jack). Tick the clock 1s at a time so sync() observes
    // the threshold crossing.
    for (int i = 0; i < 11; ++i) {
        suite->fakeClock->advance(1000);
        suite->rdc.sync(&suite->device);
    }

    // Second call: chain drained to 0
    cdm.onChainStateChanged();
    EXPECT_EQ(cdm.getBoostMs(), 0u);
    EXPECT_EQ(cdm.getConfirmedSupporterCount(), 0u);
}


// Middle-of-chain device (same-role peer on opponent jack) is NOT champion.
inline void cdmIsChampionFalseWithSameRoleOpponent(ChainManagerTests* suite) {
    suite->setupHunterChampion();
    ChainManager cdm(&suite->player, suite->device.wirelessManager, &suite->rdc);
    // Same-role peer on opponent jack → supporter, not champion.
    cdm.setPeerRole(SerialIdentifier::OUTPUT_JACK, true);  // same-role hunter
    EXPECT_TRUE(cdm.isSupporter());
    EXPECT_FALSE(cdm.isChampion());
}

// Coord election is roster-derived: derivation runs from the RDC roster
// inside sync(), so bare onDirectPeerChange firings must never claim.
inline void cdmDirectPeerConnectDoesNotClaim(ChainManagerTests* suite) {
    suite->setupHunterChampion();
    WirelessTransport transport(suite->device.wirelessManager);
    ChainManager cdm(&suite->player, suite->device.wirelessManager, &suite->rdc);
    cdm.initialize(&transport);
    suite->applyHunterChampionRoles(cdm);
    ASSERT_FALSE(cdm.isCoordinator());

    EXPECT_CALL(*suite->device.mockPeerComms, sendData(_, _, _, _)).WillRepeatedly(Return(1));
    EXPECT_CALL(*suite->device.mockPeerComms, addEspNowPeer(_)).WillRepeatedly(Return(0));

    std::array<uint8_t, 6> someMac = {0x99, 0x99, 0x99, 0x99, 0x99, 0x99};
    RemoteDeviceCoordinator::Peer peer{someMac, DeviceType::PDN};
    cdm.onDirectPeerChange(SerialIdentifier::OUTPUT_JACK,
                           std::nullopt,
                           std::optional<RemoteDeviceCoordinator::Peer>(peer));

    EXPECT_FALSE(cdm.isCoordinator());
}

// sendConfirm targets championMac_ directly with a ChainConfirmPayload.
inline void cdmSendConfirmTargetsChampionMac(ChainManagerTests* suite) {
    suite->setupHunterChampion();
    WirelessTransport transport(suite->device.wirelessManager);
    ChainManager cdm(&suite->player, suite->device.wirelessManager, &suite->rdc);
    cdm.initialize(&transport);
    // Seed championMac via announce.
    uint8_t champion[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kAck, _, _)).WillRepeatedly(Return(1));
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kRoleAnnounce, _, _)).WillRepeatedly(Return(1));
    EXPECT_CALL(*suite->device.mockPeerComms, addEspNowPeer(_)).WillRepeatedly(Return(0));
    // onRoleAnnounceReceived auto-fires sendConfirm() once championMac_ resolves
    // (load-bearing for ring loop detection). Absorb that send first, then
    // capture the explicit sendConfirm() below.
    std::array<uint8_t, 6> target{};
    ChainConfirmPayload captured{};
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kChainConfirm, _, sizeof(ChainConfirmPayload)))
        .WillRepeatedly([&](const uint8_t* mac, PktType, const uint8_t* data, const size_t) {
            memcpy(target.data(), mac, 6);
            memcpy(&captured, data, sizeof(captured));
            return 1;
        });
    cdm.onRoleAnnounceReceived(suite->opponentMac, 1, champion, 1);

    cdm.sendConfirm();

    EXPECT_EQ(memcmp(target.data(), champion, 6), 0);
    EXPECT_EQ(memcmp(captured.originatorMac, suite->localMac, 6), 0);
}

// sendConfirm increments seqId on each call.
inline void cdmSendConfirmIncrementsSeqId(ChainManagerTests* suite) {
    suite->setupHunterChampion();
    WirelessTransport transport(suite->device.wirelessManager);
    ChainManager cdm(&suite->player, suite->device.wirelessManager, &suite->rdc);
    cdm.initialize(&transport);
    uint8_t champion[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kAck, _, _)).WillRepeatedly(Return(1));
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kRoleAnnounce, _, _)).WillRepeatedly(Return(1));
    EXPECT_CALL(*suite->device.mockPeerComms, addEspNowPeer(_)).WillRepeatedly(Return(0));
    std::vector<uint8_t> seqIds;
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kChainConfirm, _, sizeof(ChainConfirmPayload)))
        .WillRepeatedly([&](const uint8_t*, PktType, const uint8_t* data, const size_t) {
            ChainConfirmPayload p; memcpy(&p, data, sizeof(p));
            seqIds.push_back(p.seqId);
            return 1;
        });

    // onRoleAnnounceReceived auto-fires sendConfirm() once championMac_ resolves.
    // Discard that initial seqId so the subsequent explicit-call assertion sees
    // the next three values in sequence.
    cdm.onRoleAnnounceReceived(suite->opponentMac, 1, champion, 1);
    ASSERT_EQ(seqIds.size(), 1u);
    uint8_t firstSeq = seqIds[0];
    seqIds.clear();

    cdm.sendConfirm();
    cdm.sendConfirm();
    cdm.sendConfirm();
    ASSERT_EQ(seqIds.size(), 3u);
    EXPECT_EQ(seqIds[0], static_cast<uint8_t>(firstSeq + 1));
    EXPECT_EQ(seqIds[1], static_cast<uint8_t>(firstSeq + 2));
    EXPECT_EQ(seqIds[2], static_cast<uint8_t>(firstSeq + 3));
}

// sendConfirm is a noop when championMac_ is not set.
inline void cdmSendConfirmNoopWhenChampionMacInvalid(ChainManagerTests* suite) {
    suite->player.setIsHunter(true);
    ChainManager cdm(&suite->player, suite->device.wirelessManager, &suite->rdc);
    // No announce received; championMac_ is nullopt.

    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kChainConfirm, _, _)).Times(0);

    cdm.sendConfirm();
}


// onRoleAnnounceReceived updates peerRoleByPort_ and championMac_, acks sender,
// registers champion as ESP-NOW peer.
inline void cdmRoleAnnounceUpdatesChampionMac(ChainManagerTests* suite) {
    suite->setupHunterChampion();
    WirelessTransport transport(suite->device.wirelessManager);
    ChainManager cdm(&suite->player, suite->device.wirelessManager, &suite->rdc);
    cdm.initialize(&transport);
    uint8_t championMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

    bool ackSent = false;
    bool peerRegistered = false;
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kAck, _, sizeof(AckPayload)))
        .WillRepeatedly([&](const uint8_t* mac, PktType, const uint8_t* data, const size_t) {
            ackSent = true;
            EXPECT_EQ(memcmp(mac, suite->opponentMac, 6), 0);
            AckPayload p;
            memcpy(&p, data, sizeof(p));
            EXPECT_EQ(p.originalType, static_cast<uint8_t>(PktType::kRoleAnnounce));
            EXPECT_EQ(p.seqId, 7u);
            return 1;
        });
    EXPECT_CALL(*suite->device.mockPeerComms, addEspNowPeer(_))
        .WillRepeatedly([&](const uint8_t* m) {
            if (memcmp(m, championMac, 6) == 0) peerRegistered = true;
            return 0;
        });
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kRoleAnnounce, _, _)).WillRepeatedly(Return(1));
    // Auto-sendConfirm fires when championMac_ resolves to a non-self peer.
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kChainConfirm, _, _)).WillRepeatedly(Return(1));

    // Route through the transport so the channel's auto-ack path fires for
    // the ackSent assertion. Direct cdm.onRoleAnnounceReceived bypasses the
    // ack emission (the channel sends it before invoking onReceive).
    RoleAnnouncePayload payload{};
    payload.role = 1;
    memcpy(payload.championMac, championMac, 6);
    payload.seqId = 7;
    transport.deliverIncoming(PktType::kRoleAnnounce, 0, suite->opponentMac,
        reinterpret_cast<const uint8_t*>(&payload), sizeof(payload));

    ASSERT_NE(cdm.getChampionMac(), nullptr);
    EXPECT_EQ(memcmp(cdm.getChampionMac(), championMac, 6), 0);
    EXPECT_TRUE(ackSent);
    EXPECT_TRUE(peerRegistered);
}

// Receiving announce with same championMac doesn't trigger cascade.
inline void cdmRoleAnnounceNoCascadeIfChampionUnchanged(ChainManagerTests* suite) {
    suite->setupHunterChampion();
    WirelessTransport transport(suite->device.wirelessManager);
    ChainManager cdm(&suite->player, suite->device.wirelessManager, &suite->rdc);
    cdm.initialize(&transport);
    uint8_t championMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kAck, _, _)).WillRepeatedly(Return(1));
    EXPECT_CALL(*suite->device.mockPeerComms, addEspNowPeer(_)).WillRepeatedly(Return(0));
    // Auto-sendConfirm fires when championMac_ resolves to a non-self peer.
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kChainConfirm, _, _)).WillRepeatedly(Return(1));
    // First receive triggers a cascade broadcast (championMac is new); may send to
    // both jacks so we allow any number >= 1.
    int firstCascadeCount = 0;
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kRoleAnnounce, _, _))
        .WillRepeatedly([&](const uint8_t*, PktType, const uint8_t*, const size_t) {
            firstCascadeCount++;
            return 1;
        });
    cdm.onRoleAnnounceReceived(suite->opponentMac, 1, championMac, 1);
    EXPECT_GE(firstCascadeCount, 1);

    // Second receive with same championMac — assert no kRoleAnnounce emit (cascade).
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kRoleAnnounce, _, _)).Times(0);

    cdm.onRoleAnnounceReceived(suite->opponentMac, 1, championMac, 2);
}

// broadcastRoleAndChampion sends to supporter-jack direct peer with current
// role and championMac.
inline void cdmBroadcastRoleAndChampionSends(ChainManagerTests* suite) {
    suite->setupHunterChampion();
    WirelessTransport transport(suite->device.wirelessManager);
    ChainManager cdm(&suite->player, suite->device.wirelessManager, &suite->rdc);
    cdm.initialize(&transport);
    suite->applyHunterChampionRoles(cdm);
    // Force championMac_ by receiving an announce.
    uint8_t champion[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    EXPECT_CALL(*suite->device.mockPeerComms, addEspNowPeer(_)).WillRepeatedly(Return(0));
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kAck, _, _)).WillRepeatedly(Return(1));
    // Allow implicit broadcast triggered inside onRoleAnnounceReceived.
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kRoleAnnounce, _, _)).WillRepeatedly(Return(1));
    cdm.onRoleAnnounceReceived(suite->opponentMac, 1, champion, 1);
    // broadcastRoleAndChampion was already called internally by onRoleAnnounceReceived.
    // Here we just verify the last broadcast had correct content by calling again.

    bool sentToSupporter = false;
    RoleAnnouncePayload capturedFromSupporter{};
    // broadcastRoleAndChampion may send to both jacks; capture the supporter-jack send.
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kRoleAnnounce, _, sizeof(RoleAnnouncePayload)))
        .WillRepeatedly([&](const uint8_t* mac, PktType, const uint8_t* data, const size_t) {
            if (memcmp(mac, suite->supporterMac, 6) == 0) {
                sentToSupporter = true;
                memcpy(&capturedFromSupporter, data, sizeof(capturedFromSupporter));
            }
            return 1;
        });

    cdm.broadcastRoleAndChampion();

    // supporter jack for hunter = INPUT_JACK; setupHunterChampion puts
    // supporterMac on INPUT.
    EXPECT_TRUE(sentToSupporter);
    EXPECT_EQ(capturedFromSupporter.role, 1u);
    EXPECT_EQ(memcmp(capturedFromSupporter.championMac, champion, 6), 0);
}

// Ack with matching seqId clears pending; subsequent sync does not retransmit.
inline void cdmAckClearsPending(ChainManagerTests* suite) {
    suite->setupHunterChampion();
    WirelessTransport transport(suite->device.wirelessManager);
    ChainManager cdm(&suite->player, suite->device.wirelessManager, &suite->rdc);
    cdm.initialize(&transport);
    suite->applyHunterChampionRoles(cdm);
    uint8_t champion[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    EXPECT_CALL(*suite->device.mockPeerComms, addEspNowPeer(_)).WillRepeatedly(Return(0));
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kAck, _, _)).WillRepeatedly(Return(1));
    // Allow implicit broadcast triggered inside onRoleAnnounceReceived.
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kRoleAnnounce, _, _)).WillRepeatedly(Return(1));
    cdm.onRoleAnnounceReceived(suite->opponentMac, 1, champion, 1);

    // Capture the seqId from the supporter-jack send (that's what pending_ tracks).
    uint8_t seqId = 0;
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kRoleAnnounce, _, sizeof(RoleAnnouncePayload)))
        .WillRepeatedly([&](const uint8_t* mac, PktType, const uint8_t* data, const size_t) {
            if (memcmp(mac, suite->supporterMac, 6) == 0) {
                RoleAnnouncePayload p; memcpy(&p, data, sizeof(p));
                seqId = p.seqId;
            }
            return 1;
        });
    cdm.broadcastRoleAndChampion();
    ASSERT_NE(seqId, 0u);

    // Ack with matching seqId clears pending. Routed through the production
    // dispatcher: ESP-NOW kAck packets land in WirelessTransport::onAckPacket.
    {
        AckPayload ack{static_cast<uint8_t>(PktType::kRoleAnnounce), 0, seqId};
        transport.onAckPacket(suite->supporterMac,
            reinterpret_cast<const uint8_t*>(&ack), sizeof(ack));
    }

    // Subsequent sync() after timeout must not emit any further role announces.
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kRoleAnnounce, _, _)).Times(0);
    suite->fakeClock->advance(150);
    transport.sync();
}

// Ack with matching seqId but wrong fromMac must not clear pending.
inline void cdmAckFromWrongMacIgnored(ChainManagerTests* suite) {
    suite->setupHunterChampion();
    WirelessTransport transport(suite->device.wirelessManager);
    ChainManager cdm(&suite->player, suite->device.wirelessManager, &suite->rdc);
    cdm.initialize(&transport);
    suite->applyHunterChampionRoles(cdm);
    uint8_t champion[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    EXPECT_CALL(*suite->device.mockPeerComms, addEspNowPeer(_)).WillRepeatedly(Return(0));
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kAck, _, _)).WillRepeatedly(Return(1));
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kRoleAnnounce, _, _)).WillRepeatedly(Return(1));
    cdm.onRoleAnnounceReceived(suite->opponentMac, 1, champion, 1);

    uint8_t seqId = 0;
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kRoleAnnounce, _, sizeof(RoleAnnouncePayload)))
        .WillRepeatedly([&](const uint8_t* mac, PktType, const uint8_t* data, const size_t) {
            if (memcmp(mac, suite->supporterMac, 6) == 0) {
                RoleAnnouncePayload p; memcpy(&p, data, sizeof(p));
                seqId = p.seqId;
            }
            return 1;
        });
    cdm.broadcastRoleAndChampion();
    ASSERT_NE(seqId, 0u);

    // Forged ack: right seqId, wrong source MAC. Must be ignored.
    uint8_t forgedMac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x00};
    {
        AckPayload ack{static_cast<uint8_t>(PktType::kRoleAnnounce), 0, seqId};
        transport.onAckPacket(forgedMac,
            reinterpret_cast<const uint8_t*>(&ack), sizeof(ack));
    }

    // sync() after timeout must still retransmit to supporterMac.
    int retransmits = 0;
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kRoleAnnounce, _, sizeof(RoleAnnouncePayload)))
        .WillRepeatedly([&](const uint8_t* mac, PktType, const uint8_t*, const size_t) {
            if (memcmp(mac, suite->supporterMac, 6) == 0) retransmits++;
            return 1;
        });
    suite->fakeClock->advance(150);
    transport.sync();
    EXPECT_EQ(retransmits, 1);
}

// Retransmit abandons after kMaxRetries with no ack.
inline void cdmRetransmitAbandonsAfterMax(ChainManagerTests* suite) {
    suite->setupHunterChampion();
    WirelessTransport transport(suite->device.wirelessManager);
    ChainManager cdm(&suite->player, suite->device.wirelessManager, &suite->rdc);
    cdm.initialize(&transport);
    suite->applyHunterChampionRoles(cdm);
    uint8_t champion[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    EXPECT_CALL(*suite->device.mockPeerComms, addEspNowPeer(_)).WillRepeatedly(Return(0));
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kAck, _, _)).WillRepeatedly(Return(1));
    // Allow implicit broadcast triggered inside onRoleAnnounceReceived.
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kRoleAnnounce, _, _)).WillRepeatedly(Return(1));
    cdm.onRoleAnnounceReceived(suite->opponentMac, 1, champion, 1);

    int supporterSends = 0;
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kRoleAnnounce, _, sizeof(RoleAnnouncePayload)))
        .WillRepeatedly([&](const uint8_t* mac, PktType, const uint8_t*, const size_t) {
            // Count sends to the supporter-jack peer (the one that goes into pending_).
            if (memcmp(mac, suite->supporterMac, 6) == 0) supporterSends++;
            return 1;
        });

    cdm.broadcastRoleAndChampion();  // initial: 1 to supporter + 1 fire-and-forget to opponent
    ASSERT_EQ(supporterSends, 1);

    // Advance clock + sync 3 times → 3 retransmits (supporter only, pending path).
    // Exponential backoff: waits are 100, 200, 400ms. Advance past the biggest
    // per iter to guarantee each retry fires.
    for (int i = 0; i < 3; i++) {
        suite->fakeClock->advance(500);
        transport.sync();
    }
    EXPECT_EQ(supporterSends, 4);  // 1 initial + 3 retransmits

    // 4th sync should not emit — pending abandoned.
    suite->fakeClock->advance(500);
    transport.sync();
    EXPECT_EQ(supporterSends, 4);
}

// RetryStats counts sends, retries, acks, and abandons across a role-announce
// lifecycle. Observability for hardware-validation tuning.
inline void cdmRetryStatsRecordsLifecycle(ChainManagerTests* suite) {
    suite->setupHunterChampion();
    WirelessTransport transport(suite->device.wirelessManager);
    ChainManager cdm(&suite->player, suite->device.wirelessManager, &suite->rdc);
    cdm.initialize(&transport);
    suite->applyHunterChampionRoles(cdm);
    uint8_t champion[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    EXPECT_CALL(*suite->device.mockPeerComms, addEspNowPeer(_)).WillRepeatedly(Return(0));
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kAck, _, _)).WillRepeatedly(Return(1));

    uint8_t seqId = 0;
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kRoleAnnounce, _, sizeof(RoleAnnouncePayload)))
        .WillRepeatedly([&](const uint8_t* mac, PktType, const uint8_t* data, const size_t) {
            if (memcmp(mac, suite->supporterMac, 6) == 0) {
                RoleAnnouncePayload p; memcpy(&p, data, sizeof(p));
                seqId = p.seqId;
            }
            return 1;
        });

    // Initial announce triggers one send, then an ack clears it.
    cdm.onRoleAnnounceReceived(suite->opponentMac, 1, champion, 1);
    ASSERT_NE(seqId, 0u);
    suite->fakeClock->advance(50);
    {
        AckPayload ack{static_cast<uint8_t>(PktType::kRoleAnnounce), 0, seqId};
        transport.onAckPacket(suite->supporterMac,
            reinterpret_cast<const uint8_t*>(&ack), sizeof(ack));
    }

    auto s1 = cdm.getRetryStats();
    EXPECT_GE(s1.sends, 1u);
    EXPECT_EQ(s1.abandons, 0u);
    EXPECT_GE(s1.ackCount, 1u);
    EXPECT_GT(s1.ackLatencyMsSum, 0u);

    // Force a retransmit then exhaust retries → abandons increments.
    // Advance generously per iter to cover exponential backoff (max 1600ms).
    cdm.broadcastRoleAndChampion();
    for (int i = 0; i < 7; i++) {
        suite->fakeClock->advance(2000);
        transport.sync();
    }
    auto s2 = cdm.getRetryStats();
    EXPECT_GE(s2.retries, 3u);
    EXPECT_GE(s2.abandons, 1u);
}

// Becoming champion self-assigns championMac to own MAC.
inline void cdmOnChainStateBecomesChampionSetsSelfMac(ChainManagerTests* suite) {
    suite->player.setIsHunter(true);
    ChainManager cdm(&suite->player, suite->device.wirelessManager, &suite->rdc);
    // Solo device — isChampion() is true by default under current semantics.
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kRoleAnnounce, _, _)).WillRepeatedly(Return(1));
    EXPECT_CALL(*suite->device.mockPeerComms, addEspNowPeer(_)).WillRepeatedly(Return(0));

    cdm.onChainStateChanged();

    ASSERT_NE(cdm.getChampionMac(), nullptr);
    EXPECT_EQ(memcmp(cdm.getChampionMac(), suite->localMac, 6), 0);
}

// A supporter that inherited championMac from an upstream announce keeps
// that cached value across onChainStateChanged; only a self-MAC value is
// cleared (see cdmChampionToSupporterClearsStaleSelfMac).
inline void cdmSupporterKeepsUpstreamChampionMacAfterTransition(ChainManagerTests* suite) {
    suite->setupHunterChampion();
    ChainManager cdm(&suite->player, suite->device.wirelessManager, &suite->rdc);

    uint8_t champion[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kAck, _, _)).WillRepeatedly(Return(1));
    EXPECT_CALL(*suite->device.mockPeerComms, addEspNowPeer(_)).WillRepeatedly(Return(0));
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kRoleAnnounce, _, _)).WillRepeatedly(Return(1));
    // Same-role opponent on OUTPUT, same-role supporter on INPUT → isSupporter.
    cdm.setPeerRole(SerialIdentifier::OUTPUT_JACK, true);
    cdm.setPeerRole(SerialIdentifier::INPUT_JACK, true);
    cdm.onRoleAnnounceReceived(suite->opponentMac, 1, champion, 1);
    ASSERT_NE(cdm.getChampionMac(), nullptr);
    ASSERT_TRUE(cdm.isSupporter());
    ASSERT_FALSE(cdm.isChampion());

    // onChainStateChanged when still supporter: championMac_ remains unchanged.
    cdm.onChainStateChanged();
    EXPECT_NE(cdm.getChampionMac(), nullptr);
}

// onChainStateChanged fires broadcast ONCE when a new supporter-jack peer appears,
// and does NOT re-fire on subsequent calls with the same peer.
inline void cdmOnChainStateNewSupporterTriggersBroadcast(ChainManagerTests* suite) {
    suite->setupHunterChampion();
    WirelessTransport transport(suite->device.wirelessManager);
    ChainManager cdm(&suite->player, suite->device.wirelessManager, &suite->rdc);
    cdm.initialize(&transport);
    suite->applyHunterChampionRoles(cdm);

    // First call: device is champion (self-assign), supporter-jack peer present →
    // should broadcast exactly once to supporter-jack.
    int firstCallSends = 0;
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kRoleAnnounce, _, sizeof(RoleAnnouncePayload)))
        .WillRepeatedly([&](const uint8_t* mac, PktType, const uint8_t*, const size_t) {
            if (memcmp(mac, suite->supporterMac, 6) == 0) firstCallSends++;
            return 1;
        });
    EXPECT_CALL(*suite->device.mockPeerComms, addEspNowPeer(_)).WillRepeatedly(Return(0));
    cdm.onChainStateChanged();
    ASSERT_TRUE(cdm.isChampion());
    ASSERT_NE(cdm.getChampionMac(), nullptr);
    EXPECT_EQ(firstCallSends, 1);

    // Second call: supporter-jack peer unchanged — must NOT re-broadcast.
    int secondCallSends = 0;
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kRoleAnnounce, _, sizeof(RoleAnnouncePayload)))
        .WillRepeatedly([&](const uint8_t* mac, PktType, const uint8_t*, const size_t) {
            if (memcmp(mac, suite->supporterMac, 6) == 0) secondCallSends++;
            return 1;
        });
    cdm.onChainStateChanged();
    EXPECT_EQ(secondCallSends, 0);
}

// End-to-end: Champion A receives a direct confirm from the originator S2
// after the join-announce cascade has propagated championMac=A to S2.
// Drives S1 (the middle supporter) to cascade the announce, and S2 to
// target A directly.
inline void chainDuelThreeDeviceConfirm(ChainManagerTests* suite) {
    suite->setupHunterChampion();
    suite->primeRosterStable();
    uint8_t macA[6] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};

    // Leg 1: S1 (middle supporter) receives announce from A, updates championMac_,
    // and cascades to its supporter-jack child.
    WirelessTransport s1Transport(suite->device.wirelessManager);
    ChainManager s1(&suite->player, suite->device.wirelessManager, &suite->rdc);
    s1.initialize(&s1Transport);
    s1.setPeerRole(SerialIdentifier::OUTPUT_JACK, true);
    s1.setPeerRole(SerialIdentifier::INPUT_JACK, true);

    std::vector<std::array<uint8_t, 6>> s1Cascade;
    RoleAnnouncePayload s1CascadePayload{};
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kAck, _, _)).WillRepeatedly(Return(1));
    EXPECT_CALL(*suite->device.mockPeerComms, addEspNowPeer(_)).WillRepeatedly(Return(0));
    // Auto-sendConfirm fires whenever championMac_ resolves to a non-self peer
    // (load-bearing for ring loop detection). Absorb both S1 and S2 auto-sends.
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kChainConfirm, _, _)).WillRepeatedly(Return(1));
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kRoleAnnounce, _, sizeof(RoleAnnouncePayload)))
        .WillRepeatedly([&](const uint8_t* mac, PktType, const uint8_t* data, const size_t) {
            std::array<uint8_t, 6> m; memcpy(m.data(), mac, 6);
            s1Cascade.push_back(m);
            memcpy(&s1CascadePayload, data, sizeof(s1CascadePayload));
            return 1;
        });

    s1.onRoleAnnounceReceived(suite->opponentMac, 1, macA, 1);
    ASSERT_GE(s1Cascade.size(), 1u);
    // S1's cascade target is its supporter-jack direct peer (supporterMac in fixture).
    EXPECT_EQ(memcmp(s1Cascade[0].data(), suite->supporterMac, 6), 0);
    EXPECT_EQ(memcmp(s1CascadePayload.championMac, macA, 6), 0);

    // Leg 2: S2 (end of chain) receives the cascaded announce, caches championMac=A.
    // In hardware, S1's MAC would be S2's opponent-jack direct peer. In this shared
    // fixture, S2's opponent-jack (OUTPUT) direct peer is opponentMac, so the announce
    // arrives from opponentMac (standing in for S1 in this single-RDC test).
    WirelessTransport s2Transport(suite->device.wirelessManager);
    ChainManager s2(&suite->player, suite->device.wirelessManager, &suite->rdc);
    s2.initialize(&s2Transport);
    s2.setPeerRole(SerialIdentifier::OUTPUT_JACK, true);

    s2.onRoleAnnounceReceived(suite->opponentMac, 1, macA, s1CascadePayload.seqId);
    ASSERT_NE(s2.getChampionMac(), nullptr);
    EXPECT_EQ(memcmp(s2.getChampionMac(), macA, 6), 0);

    // Leg 3: S2 presses button → confirm targets A directly.
    std::array<uint8_t, 6> confirmTarget{};
    ChainConfirmPayload confirmPayload{};
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kChainConfirm, _, sizeof(ChainConfirmPayload)))
        .WillOnce([&](const uint8_t* mac, PktType, const uint8_t* data, const size_t) {
            memcpy(confirmTarget.data(), mac, 6);
            memcpy(&confirmPayload, data, sizeof(confirmPayload));
            return 1;
        });

    s2.sendConfirm();

    EXPECT_EQ(memcmp(confirmTarget.data(), macA, 6), 0);  // direct to A
    EXPECT_EQ(memcmp(confirmPayload.originatorMac, suite->localMac, 6), 0);

    // Leg 4: Champion A enforces topology-membership integrity. A confirm whose
    // originator is neither A's direct supporter-jack peer nor a member of A's
    // connected component is dropped, so a device holding a stale championMac_
    // (after a reshuffle) can't inflate boost. A confirm from A's real direct
    // supporter peer is accepted.
    uint8_t distantMac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};

    WirelessTransport aTransport(suite->device.wirelessManager);
    ChainManager a(&suite->player, suite->device.wirelessManager, &suite->rdc);
    a.initialize(&aTransport);
    a.setPeerRole(SerialIdentifier::OUTPUT_JACK, false);
    a.setPeerRole(SerialIdentifier::INPUT_JACK, true);
    ASSERT_TRUE(a.isChampion());

    // Non-member, non-peer originator: dropped, boost unchanged.
    a.onConfirmReceived(suite->supporterMac, distantMac, confirmPayload.seqId);
    EXPECT_EQ(a.getBoostMs(), 0u);

    // A's real direct supporter-jack peer (supporterMac on INPUT): accepted.
    a.onConfirmReceived(suite->supporterMac, suite->supporterMac, confirmPayload.seqId);
    EXPECT_EQ(a.getBoostMs(), 15u);
}

// broadcastRoleAndChampion sends a full RoleAnnouncePayload to the
// opponent-jack direct peer so that peer can learn our role. Guards against a
// truncated payload: onRoleAnnouncePacket requires 8 bytes and silently drops
// anything shorter, so the opponent would never learn our role.
inline void cdmBroadcastToOpponentJackPopulatesRemoteRole(ChainManagerTests* suite) {
    // Hunter champion: opponent jack = OUTPUT, supporter jack = INPUT.
    suite->player.setIsHunter(true);
    suite->setupHunterChampion();

    WirelessTransport transport(suite->device.wirelessManager);
    ChainManager cdm(&suite->player, suite->device.wirelessManager, &suite->rdc);
    cdm.initialize(&transport);

    // Allow any sends triggered inside onChainStateChanged / broadcastRoleAndChampion.
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kRoleAnnounce, _, _)).WillRepeatedly(Return(1));
    EXPECT_CALL(*suite->device.mockPeerComms, addEspNowPeer(_)).WillRepeatedly(Return(0));
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kAck, _, _)).WillRepeatedly(Return(1));

    // Capture any kRoleAnnounce sent to the opponent-jack direct peer (opponentMac).
    bool sentToOpponent = false;
    RoleAnnouncePayload capturedPayload{};
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kRoleAnnounce, _, sizeof(RoleAnnouncePayload)))
        .WillRepeatedly([&](const uint8_t* mac, PktType, const uint8_t* data, const size_t) {
            if (memcmp(mac, suite->opponentMac, 6) == 0) {
                sentToOpponent = true;
                memcpy(&capturedPayload, data, sizeof(capturedPayload));
            }
            return 1;
        });

    // onChainStateChanged self-assigns champion and should broadcast to both jacks.
    cdm.onChainStateChanged();

    EXPECT_TRUE(sentToOpponent) << "No 8-byte RoleAnnouncePayload sent to opponent-jack peer";
    if (sentToOpponent) {
        EXPECT_EQ(capturedPayload.role, 1u);  // hunter
        EXPECT_EQ(memcmp(capturedPayload.championMac, suite->localMac, 6), 0);
    }
}

// When a device was champion (championMac_ == self-MAC) and gains a same-role
// peer on its opponent jack (becoming a supporter), onChainStateChanged must
// clear championMac_ rather than cascade the stale self-MAC to its supporter.
inline void cdmChampionToSupporterClearsStaleSelfMac(ChainManagerTests* suite) {
    suite->player.setIsHunter(true);
    ChainManager cdm(&suite->player, suite->device.wirelessManager, &suite->rdc);

    // Allow all sends; connectOutputPort triggers handshake sends too.
    EXPECT_CALL(*suite->device.mockPeerComms, sendData(_, _, _, _)).WillRepeatedly(Return(1));
    EXPECT_CALL(*suite->device.mockPeerComms, addEspNowPeer(_)).WillRepeatedly(Return(0));

    // Start as solo champion — self-assigns championMac_ to localMac.
    cdm.onChainStateChanged();
    ASSERT_NE(cdm.getChampionMac(), nullptr);
    EXPECT_EQ(memcmp(cdm.getChampionMac(), suite->localMac, 6), 0);

    // Connect output port so there is a direct peer on opponent jack.
    suite->connectOutputPort();
    // Mark that peer as same-role (hunter) → device becomes supporter.
    cdm.setPeerRole(SerialIdentifier::OUTPUT_JACK, true);
    ASSERT_TRUE(cdm.isSupporter());

    // onChainStateChanged should detect we hold stale self-MAC and clear it.
    cdm.onChainStateChanged();

    EXPECT_EQ(cdm.getChampionMac(), nullptr)
        << "Stale self-MAC was not cleared when champion was demoted to supporter";
}

// Role announce received on supporter-jack (child direction) updates role
// but does NOT update championMac_. Prevents ping-pong between same-role peers.
inline void cdmRoleAnnounceFromSupporterJackIgnoresChampionMac(ChainManagerTests* suite) {
    suite->setupHunterChampion();
    ChainManager cdm(&suite->player, suite->device.wirelessManager, &suite->rdc);
    // Seed championMac via opponent-jack announce first.
    uint8_t realChampion[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    EXPECT_CALL(*suite->device.mockPeerComms, addEspNowPeer(_)).WillRepeatedly(Return(0));
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kAck, _, _)).WillRepeatedly(Return(1));
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kRoleAnnounce, _, _)).WillRepeatedly(Return(1));
    cdm.onRoleAnnounceReceived(suite->opponentMac, 1, realChampion, 1);
    ASSERT_NE(cdm.getChampionMac(), nullptr);
    EXPECT_EQ(memcmp(cdm.getChampionMac(), realChampion, 6), 0);

    // Now a supporter-jack peer sends an announce with a different championMac
    // (simulating the ping-pong scenario). peerRoleByPort for supporter-jack
    // should update, but championMac_ should NOT change.
    uint8_t wrongChampion[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    cdm.onRoleAnnounceReceived(suite->supporterMac, 1, wrongChampion, 2);

    // championMac_ unchanged
    EXPECT_EQ(memcmp(cdm.getChampionMac(), realChampion, 6), 0);
}

// Role announce from opposite-role opponent-jack peer does NOT update championMac_.
inline void cdmRoleAnnounceFromOppositeRoleOpponentIgnoresChampionMac(ChainManagerTests* suite) {
    suite->setupHunterChampion();
    ChainManager cdm(&suite->player, suite->device.wirelessManager, &suite->rdc);
    suite->applyHunterChampionRoles(cdm);
    uint8_t selfMacArr[6];
    memcpy(selfMacArr, suite->localMac, 6);

    // Seed championMac_ to self via onChainStateChanged (champion self-assign).
    EXPECT_CALL(*suite->device.mockPeerComms, addEspNowPeer(_)).WillRepeatedly(Return(0));
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kRoleAnnounce, _, _)).WillRepeatedly(Return(1));
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kAck, _, _)).WillRepeatedly(Return(1));
    cdm.onChainStateChanged();
    ASSERT_NE(cdm.getChampionMac(), nullptr);
    EXPECT_EQ(memcmp(cdm.getChampionMac(), selfMacArr, 6), 0);

    // Receive announce from bounty (role=0) on opponent jack. Hunter champion
    // should ignore this — opposite-role sender is a duel opponent, not a chain parent.
    uint8_t bountyChampion[6] = {0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB};
    cdm.onRoleAnnounceReceived(suite->opponentMac, /*role=bounty*/0, bountyChampion, 7);

    // championMac_ must remain self (not overwritten with opponent's champion MAC).
    ASSERT_NE(cdm.getChampionMac(), nullptr);
    EXPECT_EQ(memcmp(cdm.getChampionMac(), selfMacArr, 6), 0);
}

// After chain break (S1 disconnects from A), S1 becomes champion
// and updates championMac_ to self.
inline void chainDuelReconfigRecovers(ChainManagerTests* suite) {
    suite->setupHunterChampion();
    uint8_t macA[6] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};

    // S1 starts with championMac=A (received from parent).
    ChainManager s1(&suite->player, suite->device.wirelessManager, &suite->rdc);
    s1.setPeerRole(SerialIdentifier::OUTPUT_JACK, true);
    s1.setPeerRole(SerialIdentifier::INPUT_JACK, true);

    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kAck, _, _)).WillRepeatedly(Return(1));
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kHandshakeCommand, _, _)).WillRepeatedly(Return(1));
    EXPECT_CALL(*suite->device.mockPeerComms, addEspNowPeer(_)).WillRepeatedly(Return(0));
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kRoleAnnounce, _, _)).WillRepeatedly(Return(1));

    s1.onRoleAnnounceReceived(suite->opponentMac, 1, macA, 1);
    ASSERT_NE(s1.getChampionMac(), nullptr);
    EXPECT_EQ(memcmp(s1.getChampionMac(), macA, 6), 0);

    // Simulate S1 losing opponent-jack peer by tripping the silent-link
    // jack-dead path. The fixture defaults silent-link to 60s so primeRoster
    // doesn't surrender mid-prime; restore the 2s production threshold here.
    suite->rdc.setJackDeadSilentLinkMsForTest(2000);
    for (int i = 0; i < 11; ++i) {
        suite->fakeClock->advance(1000);
        suite->rdc.sync(&suite->device);
    }
    // Now opponent-jack peer is gone; re-evaluate on CDM.
    s1.onChainStateChanged();

    ASSERT_TRUE(s1.isChampion());
    ASSERT_NE(s1.getChampionMac(), nullptr);
    EXPECT_EQ(memcmp(s1.getChampionMac(), suite->localMac, 6), 0);
}

// COUNTDOWN is fire-and-forget: seqId must be 0 and no retry must fire.
inline void cdmGameEventCountdownIsFireAndForget(ChainManagerTests* suite) {
    suite->setupHunterChampion();
    WirelessTransport transport(suite->device.wirelessManager);
    ChainManager cdm(&suite->player, suite->device.wirelessManager, &suite->rdc);
    cdm.initialize(&transport);
    suite->applyHunterChampionRoles(cdm);

    int countdownSends = 0;
    uint8_t seqId = 0xAB;
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kChainGameEvent, _, sizeof(ChainGameEventPayload)))
        .WillRepeatedly([&](const uint8_t*, PktType, const uint8_t* data, const size_t) {
            ChainGameEventPayload p; memcpy(&p, data, sizeof(p));
            if (p.event_type == (uint8_t)ChainGameEventType::COUNTDOWN) {
                seqId = p.seqId;
                countdownSends++;
            }
            return 1;
        });
    cdm.sendGameEventToSupporters(ChainGameEventType::COUNTDOWN);
    EXPECT_EQ(countdownSends, 1);
    EXPECT_EQ(seqId, 0u);  // fire-and-forget sentinel

    // sync() after timeout must not retransmit a COUNTDOWN.
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kChainGameEvent, _, _)).Times(0);
    suite->fakeClock->advance(1000);
    transport.sync();
}

// WIN is tracked: seqId != 0, pending registered, retransmit fires on timer.
inline void cdmGameEventWinIsTrackedAndRetried(ChainManagerTests* suite) {
    suite->setupHunterChampion();
    WirelessTransport transport(suite->device.wirelessManager);
    ChainManager cdm(&suite->player, suite->device.wirelessManager, &suite->rdc);
    cdm.initialize(&transport);
    suite->applyHunterChampionRoles(cdm);

    int supporterSends = 0;
    uint8_t winSeqId = 0;
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kChainGameEvent, _, sizeof(ChainGameEventPayload)))
        .WillRepeatedly([&](const uint8_t* mac, PktType, const uint8_t* data, const size_t) {
            if (memcmp(mac, suite->supporterMac, 6) == 0) {
                ChainGameEventPayload p; memcpy(&p, data, sizeof(p));
                if (p.event_type == (uint8_t)ChainGameEventType::WIN) {
                    winSeqId = p.seqId;
                    supporterSends++;
                }
            }
            return 1;
        });

    cdm.sendGameEventToSupporters(ChainGameEventType::WIN);
    EXPECT_EQ(supporterSends, 1);
    ASSERT_NE(winSeqId, 0u);

    // Advance past first timeout (100ms), sync() should retransmit once.
    suite->fakeClock->advance(150);
    transport.sync();
    EXPECT_EQ(supporterSends, 2);
}

// ACK clears pending: no further retransmits after a matching ACK.
inline void cdmGameEventAckClearsPending(ChainManagerTests* suite) {
    suite->setupHunterChampion();
    WirelessTransport transport(suite->device.wirelessManager);
    ChainManager cdm(&suite->player, suite->device.wirelessManager, &suite->rdc);
    cdm.initialize(&transport);
    suite->applyHunterChampionRoles(cdm);

    uint8_t winSeqId = 0;
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kChainGameEvent, _, sizeof(ChainGameEventPayload)))
        .WillRepeatedly([&](const uint8_t* mac, PktType, const uint8_t* data, const size_t) {
            if (memcmp(mac, suite->supporterMac, 6) == 0) {
                ChainGameEventPayload p; memcpy(&p, data, sizeof(p));
                if (p.event_type == (uint8_t)ChainGameEventType::LOSS) winSeqId = p.seqId;
            }
            return 1;
        });

    cdm.sendGameEventToSupporters(ChainGameEventType::LOSS);
    ASSERT_NE(winSeqId, 0u);

    {
        AckPayload ack{static_cast<uint8_t>(PktType::kChainGameEvent), 0, winSeqId};
        transport.onAckPacket(suite->supporterMac,
            reinterpret_cast<const uint8_t*>(&ack), sizeof(ack));
    }

    // After ACK, sync past timeout must not retransmit.
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kChainGameEvent, _, _)).Times(0);
    suite->fakeClock->advance(1000);
    transport.sync();
}

// After kMaxRetries with no ACK, pending is abandoned — no further sends.
inline void cdmGameEventAbandonsAfterMax(ChainManagerTests* suite) {
    suite->setupHunterChampion();
    WirelessTransport transport(suite->device.wirelessManager);
    ChainManager cdm(&suite->player, suite->device.wirelessManager, &suite->rdc);
    cdm.initialize(&transport);
    suite->applyHunterChampionRoles(cdm);

    int supporterSends = 0;
    EXPECT_CALL(*suite->device.mockPeerComms,
                sendData(_, PktType::kChainGameEvent, _, sizeof(ChainGameEventPayload)))
        .WillRepeatedly([&](const uint8_t* mac, PktType, const uint8_t*, const size_t) {
            if (memcmp(mac, suite->supporterMac, 6) == 0) supporterSends++;
            return 1;
        });

    cdm.sendGameEventToSupporters(ChainGameEventType::WIN);
    ASSERT_EQ(supporterSends, 1);

    // kMaxRetries = 3. Advance past each exponential backoff window (100, 200,
    // 400, 800ms). Use 1000ms to cover the largest.
    for (int i = 0; i < 3; i++) {
        suite->fakeClock->advance(1000);
        transport.sync();
    }
    EXPECT_EQ(supporterSends, 4);  // 1 initial + 3 retransmits

    // After abandon, further sync must not retransmit.
    suite->fakeClock->advance(1000);
    transport.sync();
    EXPECT_EQ(supporterSends, 4);

    EXPECT_EQ(cdm.getRetryStats().abandons, 1u);
}

// =====================================================================
// Roster-derived coord election with 1-cycle stability guard.
//
// (FakeRosterRDC is defined near the top of this file, right after the
// fixture, so the loop-gated canInitiateMatch test can use it too.)
// =====================================================================

// Self with lowest MAC + isInLoop + min stable for ≥1 cycle → claim coord.
inline void cdmClaimsCoordinatorWhenSelfIsLowestMacInLoop(ChainManagerTests* suite) {
    FakeRosterRDC fakeRdc;
    WirelessTransport transport(suite->device.wirelessManager);
    ChainManager cdm(&suite->player, suite->device.wirelessManager, &fakeRdc);
    cdm.initialize(&transport);
    EXPECT_CALL(*suite->device.mockPeerComms, addEspNowPeer(_)).WillRepeatedly(Return(0));
    EXPECT_CALL(*suite->device.mockPeerComms, sendData(_, _, _, _)).WillRepeatedly(Return(1));

    // Self (localMac = 11:22:33:44:55:66) is the lowest MAC.
    net::Mac self;
    memcpy(self.data(), suite->localMac, 6);
    net::Mac higher = {0xAA, 0x00, 0x00, 0x00, 0x00, 0x01};
    fakeRdc.members = {self, higher};
    fakeRdc.inLoop = true;

    // First sync seeds lastStableMin_; not yet eligible to claim.
    cdm.sync();
    EXPECT_FALSE(cdm.isCoordinator()) << "Cycle 0 seeds the value; must not claim yet";

    // Advance past one PROBE cycle. Now min has been stable for >=1 cycle.
    suite->fakeClock->advance(1100);
    cdm.sync();
    EXPECT_TRUE(cdm.isCoordinator()) << "After 1s of stability, self lowest → claim";
}

// A lower-MAC peer arriving in the roster must demote a sitting coordinator.
inline void cdmDemotesCoordinatorWhenLowerMacJoins(ChainManagerTests* suite) {
    FakeRosterRDC fakeRdc;
    WirelessTransport transport(suite->device.wirelessManager);
    ChainManager cdm(&suite->player, suite->device.wirelessManager, &fakeRdc);
    cdm.initialize(&transport);
    EXPECT_CALL(*suite->device.mockPeerComms, addEspNowPeer(_)).WillRepeatedly(Return(0));
    EXPECT_CALL(*suite->device.mockPeerComms, sendData(_, _, _, _)).WillRepeatedly(Return(1));

    net::Mac self;
    memcpy(self.data(), suite->localMac, 6);
    net::Mac higher = {0xAA, 0x00, 0x00, 0x00, 0x00, 0x01};
    fakeRdc.members = {self, higher};
    fakeRdc.inLoop = true;

    // Drive to coord-claim.
    cdm.sync();
    suite->fakeClock->advance(1100);
    cdm.sync();
    ASSERT_TRUE(cdm.isCoordinator());

    // A new device with a MAC lower than self joins the roster.
    net::Mac lower = {0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    fakeRdc.members = {self, higher, lower};

    suite->fakeClock->advance(1100);
    cdm.sync();
    EXPECT_FALSE(cdm.isCoordinator())
        << "Lower-MAC peer arrival must demote — defers coord to that peer";
}

// Linear chain (isInLoop=false) — never claim coord regardless of MAC ordering.
inline void cdmDoesNotClaimWithoutLoop(ChainManagerTests* suite) {
    FakeRosterRDC fakeRdc;
    WirelessTransport transport(suite->device.wirelessManager);
    ChainManager cdm(&suite->player, suite->device.wirelessManager, &fakeRdc);
    cdm.initialize(&transport);
    EXPECT_CALL(*suite->device.mockPeerComms, addEspNowPeer(_)).WillRepeatedly(Return(0));
    EXPECT_CALL(*suite->device.mockPeerComms, sendData(_, _, _, _)).WillRepeatedly(Return(1));

    net::Mac self;
    memcpy(self.data(), suite->localMac, 6);
    net::Mac higher = {0xAA, 0x00, 0x00, 0x00, 0x00, 0x01};
    fakeRdc.members = {self, higher};
    fakeRdc.inLoop = false;  // linear, not a ring

    for (int i = 0; i < 5; ++i) {
        suite->fakeClock->advance(1100);
        cdm.sync();
    }
    EXPECT_FALSE(cdm.isCoordinator()) << "Linear chain must never produce a coord";
}

// Min-MAC flips (self → other → self) within one PROBE cycle. The 1-cycle
// stability guard must prevent any coord claim during the churn window.
inline void cdmOneSecondMinStabilityGuard(ChainManagerTests* suite) {
    FakeRosterRDC fakeRdc;
    WirelessTransport transport(suite->device.wirelessManager);
    ChainManager cdm(&suite->player, suite->device.wirelessManager, &fakeRdc);
    cdm.initialize(&transport);
    EXPECT_CALL(*suite->device.mockPeerComms, addEspNowPeer(_)).WillRepeatedly(Return(0));
    EXPECT_CALL(*suite->device.mockPeerComms, sendData(_, _, _, _)).WillRepeatedly(Return(1));

    net::Mac self;
    memcpy(self.data(), suite->localMac, 6);
    net::Mac lower = {0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    net::Mac higher = {0xAA, 0x00, 0x00, 0x00, 0x00, 0x01};
    fakeRdc.inLoop = true;

    // Tick 1: self is lowest (no `lower` yet).
    fakeRdc.members = {self, higher};
    cdm.sync();
    EXPECT_FALSE(cdm.isCoordinator()) << "Cycle 0 seeds, never claims";

    // Tick 2: lower joins — min flips to `lower`. Reset cycle counter.
    suite->fakeClock->advance(1100);
    fakeRdc.members = {self, higher, lower};
    cdm.sync();
    EXPECT_FALSE(cdm.isCoordinator()) << "min flipped; counter resets — no claim";

    // Tick 3: lower leaves — min flips back to self. Reset cycle counter again.
    suite->fakeClock->advance(1100);
    fakeRdc.members = {self, higher};
    cdm.sync();
    EXPECT_FALSE(cdm.isCoordinator())
        << "Stability guard: min just flipped back, cycle counter is fresh";

    // Tick 4: min still self. Stability counter hits 1 → eligible to claim.
    suite->fakeClock->advance(1100);
    cdm.sync();
    EXPECT_TRUE(cdm.isCoordinator()) << "Min stable for one cycle → claim allowed";
}

// onConfirmReceived applies the isTopologyStable gate unconditionally: a confirm
// arriving while the roster is still settling is deferred until the topology
// holds for the stability window, so partial bracket assembly mid-convergence
// can't latch a stale supporter count. This case drives a ring member.
inline void cdmConfirmDroppedWhenRosterUnstable(ChainManagerTests* suite) {
    FakeRosterRDC fakeRdc;
    // The confirming device is a member of our ring; topology-membership
    // integrity (see onConfirmReceived) requires it before the stability gate
    // is even reached.
    net::Mac supMember; std::copy_n(suite->supporterMac, 6, supMember.begin());
    fakeRdc.members.push_back(supMember);
    WirelessTransport transport(suite->device.wirelessManager);
    ChainManager cdm(&suite->player, suite->device.wirelessManager, &fakeRdc);
    cdm.initialize(&transport);

    fakeRdc.inLoop = true;
    fakeRdc.stable = false;
    cdm.onConfirmReceived(suite->supporterMac, suite->supporterMac, 1);
    EXPECT_EQ(cdm.getConfirmedSupporterCount(), 0u)
        << "In a ring, confirms must be deferred while roster is unstable";

    fakeRdc.stable = true;
    cdm.onConfirmReceived(suite->supporterMac, suite->supporterMac, 1);
    EXPECT_EQ(cdm.getConfirmedSupporterCount(), 1u)
        << "Once stable, the ring's confirm is accepted";
}

