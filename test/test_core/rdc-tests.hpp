#pragma once

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "device-mock.hpp"
#include "utility-tests.hpp"
#include "device/remote-device-coordinator.hpp"
#include "device/peer-graph-codec.hpp"
#include "device/device-constants.hpp"

using ::testing::Return;
using ::testing::_;

// ============================================
// RemoteDeviceCoordinator Tests (peer-graph protocol)
// ============================================

class RDCTests : public testing::Test {
public:
    void SetUp() override {
        fakeClock = new FakePlatformClock();
        SimpleTimer::setPlatformClock(fakeClock);
        fakeClock->setTime(1000);

        ON_CALL(*device.mockPeerComms, sendData(_, _, _, _)).WillByDefault(Return(1));
        ON_CALL(*device.mockPeerComms, getMacAddress()).WillByDefault(Return(localMac));
        ON_CALL(*device.mockPeerComms, addEspNowPeer(_)).WillByDefault(Return(0));
        ON_CALL(*device.mockPeerComms, removeEspNowPeer(_)).WillByDefault(Return(0));
        ON_CALL(*device.mockPeerComms, getPeerCommsState()).WillByDefault(Return(PeerCommsState::CONNECTED));

        rdc.initialize(device.wirelessManager, device.serialManager, &device);
        // Most single-device tests advance the fake clock across windows far
        // longer than the 30ms HELLO silent-link. Push the threshold out so
        // silent-link doesn't fire unless a test specifically exercises it.
        rdc.setJackDeadSilentLinkMsForTest(60000);
    }

    void TearDown() override {
        SimpleTimer::setPlatformClock(nullptr);
        delete fakeClock;
    }

    FakeHWSerialWrapper& serialFor(SerialIdentifier jack) {
        return jack == SerialIdentifier::INPUT_JACK
            ? device.inputJackSerial : device.outputJackSerial;
    }

    void deliverHello(SerialIdentifier jack, const uint8_t* mac,
                      uint8_t deviceType = static_cast<uint8_t>(DeviceType::PDN)) {
        net::Mac m;
        std::copy_n(mac, 6, m.begin());
        auto frame = peer_graph::encodeHello(m, deviceType);
        serialFor(jack).bytesCallback(frame.data(), frame.size());
    }

    void deliverBeacon(SerialIdentifier jack, const BeaconRecord& b) {
        auto frame = peer_graph::encodeBeacon(b);
        serialFor(jack).bytesCallback(frame.data(), frame.size());
        // ingestSerial only enqueues; exec() drains it so the BEACON is accepted
        // and flooded within this helper, without driving a full sync().
        rdc.exec();
    }

    // True if a BEACON frame from `source` claiming (inPeer, outPeer) appears in
    // the bytes RDC emitted on `jack` since the last queue drain.
    bool emittedBeacon(SerialIdentifier jack, const BeaconRecord& want) {
        auto& q = serialFor(jack).msgQueue;
        std::vector<uint8_t> bytes(q.begin(), q.end());
        const auto target = peer_graph::encodeBeacon(want);
        if (bytes.size() < target.size()) return false;
        for (size_t i = 0; i + target.size() <= bytes.size(); ++i) {
            if (std::equal(target.begin(), target.end(), bytes.begin() + i)) return true;
        }
        return false;
    }

    void drainQueues() {
        device.inputJackSerial.msgQueue.clear();
        device.outputJackSerial.msgQueue.clear();
    }

    net::Mac mac(uint8_t last) { return {0x0A, 0, 0, 0, 0, last}; }

    MockDevice device;
    RemoteDeviceCoordinator rdc;
    FakePlatformClock* fakeClock;
    uint8_t localMac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
};

inline void rdcDefaultStateIsDisconnected(RDCTests* suite) {
    EXPECT_EQ(suite->rdc.getPortStatus(SerialIdentifier::OUTPUT_JACK), PortStatus::DISCONNECTED);
    EXPECT_EQ(suite->rdc.getPortStatus(SerialIdentifier::INPUT_JACK), PortStatus::DISCONNECTED);
    EXPECT_EQ(suite->rdc.getPeerMac(SerialIdentifier::OUTPUT_JACK), nullptr);
    EXPECT_FALSE(suite->rdc.isInLoop());
    EXPECT_EQ(suite->rdc.getChainMembers().size(), 1u);  // just self
}

inline void rdcHelloSetsMacPeerAndConnected(RDCTests* suite) {
    uint8_t peer[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    suite->deliverHello(SerialIdentifier::OUTPUT_JACK, peer);
    suite->rdc.sync(&suite->device);
    EXPECT_EQ(suite->rdc.getPortStatus(SerialIdentifier::OUTPUT_JACK), PortStatus::CONNECTED);
    const uint8_t* m = suite->rdc.getPeerMac(SerialIdentifier::OUTPUT_JACK);
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m[0], 0xAA);
    EXPECT_EQ(m[5], 0xFF);
    // INPUT untouched.
    EXPECT_EQ(suite->rdc.getPortStatus(SerialIdentifier::INPUT_JACK), PortStatus::DISCONNECTED);
}

inline void rdcHelloFromSelfRejected(RDCTests* suite) {
    suite->deliverHello(SerialIdentifier::OUTPUT_JACK, suite->localMac);
    suite->rdc.sync(&suite->device);
    EXPECT_EQ(suite->rdc.getPeerMac(SerialIdentifier::OUTPUT_JACK), nullptr);
}

inline void rdcSilentLinkClearsPeerAfterThreshold(RDCTests* suite) {
    suite->rdc.setJackDeadSilentLinkMsForTest(30);
    uint8_t peer[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    suite->deliverHello(SerialIdentifier::OUTPUT_JACK, peer);
    suite->rdc.sync(&suite->device);
    ASSERT_NE(suite->rdc.getPeerMac(SerialIdentifier::OUTPUT_JACK), nullptr);
    // No further HELLO; advance past 30ms.
    suite->fakeClock->advance(40);
    suite->rdc.sync(&suite->device);
    EXPECT_EQ(suite->rdc.getPeerMac(SerialIdentifier::OUTPUT_JACK), nullptr);
    EXPECT_EQ(suite->rdc.getPortStatus(SerialIdentifier::OUTPUT_JACK), PortStatus::DISCONNECTED);
}

// A peer that silent-dies must release its ESP-NOW slot. The 20-slot table is
// finite; without this every distinct neighbour that drops leaks a slot until
// new peers are rejected and matches silently fail.
inline void rdcSilentLinkReleasesEspNowPeerSlot(RDCTests* suite) {
    suite->rdc.setJackDeadSilentLinkMsForTest(30);
    uint8_t peer[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    suite->deliverHello(SerialIdentifier::OUTPUT_JACK, peer);
    suite->rdc.sync(&suite->device);
    ASSERT_NE(suite->rdc.getPeerMac(SerialIdentifier::OUTPUT_JACK), nullptr);

    EXPECT_CALL(*suite->device.mockPeerComms,
                removeEspNowPeer(::testing::Truly([&](const uint8_t* m) {
                    return m != nullptr && memcmp(m, peer, 6) == 0;
                }))).Times(1);

    // No further HELLO; silent-link fires declareJackDead on the next sync.
    suite->fakeClock->advance(40);
    suite->rdc.sync(&suite->device);
}

inline void rdcSilentLinkSurvivesRefreshWithinWindow(RDCTests* suite) {
    // 100ms mirrors the production kHelloSilentLinkMs. Each HELLO inside the
    // window resets the liveness baseline, so a peer that keeps emitting never
    // trips the watchdog; only a full window of true silence declares it dead.
    // Guards the false-fire margin: a sub-threshold gap must not drop the peer.
    suite->rdc.setJackDeadSilentLinkMsForTest(100);
    uint8_t peer[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    suite->deliverHello(SerialIdentifier::OUTPUT_JACK, peer);
    suite->rdc.sync(&suite->device);
    ASSERT_NE(suite->rdc.getPeerMac(SerialIdentifier::OUTPUT_JACK), nullptr);

    // 80ms gap (< window), then a refreshing HELLO: baseline resets, still alive.
    suite->fakeClock->advance(80);
    suite->deliverHello(SerialIdentifier::OUTPUT_JACK, peer);
    suite->rdc.sync(&suite->device);
    EXPECT_NE(suite->rdc.getPeerMac(SerialIdentifier::OUTPUT_JACK), nullptr);

    // Another 80ms gap — still under 100ms since the last HELLO: alive.
    suite->fakeClock->advance(80);
    suite->rdc.sync(&suite->device);
    EXPECT_NE(suite->rdc.getPeerMac(SerialIdentifier::OUTPUT_JACK), nullptr);

    // Now go fully silent past the window: dead.
    suite->fakeClock->advance(120);
    suite->rdc.sync(&suite->device);
    EXPECT_EQ(suite->rdc.getPeerMac(SerialIdentifier::OUTPUT_JACK), nullptr);
}

inline void rdcSilentLinkFiresDisconnectCallback(RDCTests* suite) {
    suite->rdc.setJackDeadSilentLinkMsForTest(30);
    int disconnects = 0;
    suite->rdc.setOnDirectPeerChange(
        [&](SerialIdentifier, std::optional<RemoteDeviceCoordinator::Peer> prev,
            std::optional<RemoteDeviceCoordinator::Peer> cur) {
            if (prev.has_value() && !cur.has_value()) disconnects++;
        });
    uint8_t peer[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    suite->deliverHello(SerialIdentifier::OUTPUT_JACK, peer);
    suite->rdc.sync(&suite->device);
    suite->fakeClock->advance(40);
    suite->rdc.sync(&suite->device);
    EXPECT_EQ(disconnects, 1);
}

inline void rdcConnectFiresConnectCallback(RDCTests* suite) {
    int connects = 0;
    suite->rdc.setOnDirectPeerChange(
        [&](SerialIdentifier, std::optional<RemoteDeviceCoordinator::Peer> prev,
            std::optional<RemoteDeviceCoordinator::Peer> cur) {
            if (!prev.has_value() && cur.has_value()) connects++;
        });
    uint8_t peer[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    suite->deliverHello(SerialIdentifier::OUTPUT_JACK, peer);
    suite->rdc.sync(&suite->device);
    EXPECT_EQ(connects, 1);
}

inline void rdcIsDirectPeerTrueForCableNeighbor(RDCTests* suite) {
    uint8_t peer[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    suite->deliverHello(SerialIdentifier::INPUT_JACK, peer);
    suite->rdc.sync(&suite->device);
    EXPECT_TRUE(suite->rdc.isDirectPeer(peer));
    uint8_t stranger[6] = {0x99, 0, 0, 0, 0, 0};
    EXPECT_FALSE(suite->rdc.isDirectPeer(stranger));
}

inline void rdcMacPeerChangeEmitsBeacon(RDCTests* suite) {
    suite->drainQueues();
    uint8_t peer[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    suite->deliverHello(SerialIdentifier::OUTPUT_JACK, peer);
    suite->rdc.sync(&suite->device);
    // reconcileSelfPeers should have emitted a BEACON claiming the peer on OUT.
    BeaconRecord want;
    std::copy_n(suite->localMac, 6, want.source.begin());
    std::copy_n(peer, 6, want.outPeer.begin());  // inPeer stays zero
    EXPECT_TRUE(suite->emittedBeacon(SerialIdentifier::OUTPUT_JACK, want));
    EXPECT_TRUE(suite->emittedBeacon(SerialIdentifier::INPUT_JACK, want));
}

// A 3-device ring: self + two peers each claiming around the ring → isInLoop.
inline void rdcBeaconsFormRingIsInLoop(RDCTests* suite) {
    // Self's direct peers: 0x0A..02 on OUTPUT, 0x0A..03 on INPUT.
    suite->deliverHello(SerialIdentifier::OUTPUT_JACK, suite->mac(0x02).data());
    suite->deliverHello(SerialIdentifier::INPUT_JACK, suite->mac(0x03).data());
    suite->rdc.sync(&suite->device);
    // Peer 0x02 claims self (on its IN) and 0x03 (on its OUT).
    net::Mac self;
    std::copy_n(suite->localMac, 6, self.begin());
    suite->deliverBeacon(SerialIdentifier::OUTPUT_JACK,
                         {suite->mac(0x02), suite->mac(0x03), self});
    // Peer 0x03 claims 0x02 (IN) and self (OUT).
    suite->deliverBeacon(SerialIdentifier::INPUT_JACK,
                         {suite->mac(0x03), suite->mac(0x02), self});
    suite->rdc.sync(&suite->device);
    EXPECT_TRUE(suite->rdc.isInLoop());
    EXPECT_EQ(suite->rdc.getChainMembers().size(), 3u);
}

inline void rdcSelfSourcedBeaconNotForwarded(RDCTests* suite) {
    suite->drainQueues();
    net::Mac self;
    std::copy_n(suite->localMac, 6, self.begin());
    // A beacon claiming to originate from self arrives (our own, looped around).
    suite->deliverBeacon(SerialIdentifier::OUTPUT_JACK,
                         {self, suite->mac(0x02), suite->mac(0x03)});
    // It must not be re-emitted on the opposite (INPUT) jack.
    auto& inQ = suite->device.inputJackSerial.msgQueue;
    std::vector<uint8_t> bytes(inQ.begin(), inQ.end());
    BeaconRecord echoed{self, suite->mac(0x02), suite->mac(0x03)};
    auto target = peer_graph::encodeBeacon(echoed);
    bool found = false;
    for (size_t i = 0; i + target.size() <= bytes.size(); ++i)
        if (std::equal(target.begin(), target.end(), bytes.begin() + i)) found = true;
    EXPECT_FALSE(found);
}

inline void rdcForeignBeaconFloodedOnOppositeJack(RDCTests* suite) {
    suite->drainQueues();
    BeaconRecord b{suite->mac(0x02), suite->mac(0x03), suite->mac(0x04)};
    suite->deliverBeacon(SerialIdentifier::OUTPUT_JACK, b);
    // Flooded onward on INPUT (opposite jack).
    auto& inQ = suite->device.inputJackSerial.msgQueue;
    std::vector<uint8_t> bytes(inQ.begin(), inQ.end());
    auto target = peer_graph::encodeBeacon(b);
    bool found = false;
    for (size_t i = 0; i + target.size() <= bytes.size(); ++i)
        if (std::equal(target.begin(), target.end(), bytes.begin() + i)) found = true;
    EXPECT_TRUE(found);
}
