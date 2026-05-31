#pragma once

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "device-mock.hpp"
#include "wireless/handshake-wireless-manager.hpp"
#include "device/device-constants.hpp"

using ::testing::Return;
using ::testing::_;
using ::testing::NiceMock;

inline Peer makeTestPeer(const uint8_t* mac, SerialIdentifier sid) {
    Peer p;
    std::copy(mac, mac + 6, p.macAddr.begin());
    p.sid = sid;
    p.deviceType = DeviceType::PDN;
    return p;
}

class HWMUnitTests : public testing::Test {
public:
    void SetUp() override {
        ON_CALL(peerComms, sendData(_, _, _, _)).WillByDefault(Return(1));
        wirelessManager = new WirelessManager(&peerComms, &httpClient);
        hwm.initialize(wirelessManager);
    }

    void TearDown() override {
        delete wirelessManager;
    }

    NiceMock<MockPeerComms> peerComms;
    MockHttpClient httpClient;
    WirelessManager* wirelessManager = nullptr;
    HandshakeWirelessManager hwm;
};

inline void hwmGetMacPeerReturnsNullWhenNotSet(HWMUnitTests* suite) {
    EXPECT_EQ(suite->hwm.getMacPeer(SerialIdentifier::OUTPUT_JACK), nullptr);
    EXPECT_EQ(suite->hwm.getMacPeer(SerialIdentifier::INPUT_JACK), nullptr);
}

inline void hwmGetMacPeerReturnsCorrectMac(HWMUnitTests* suite) {
    uint8_t mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    suite->hwm.setMacPeer(SerialIdentifier::OUTPUT_JACK, makeTestPeer(mac, SerialIdentifier::OUTPUT_JACK));

    const Peer* result = suite->hwm.getMacPeer(SerialIdentifier::OUTPUT_JACK);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->macAddr[0], 0x11);
    EXPECT_EQ(result->macAddr[5], 0x66);

    EXPECT_EQ(suite->hwm.getMacPeer(SerialIdentifier::INPUT_JACK), nullptr);
}

inline void hwmRemoveMacPeerClearsEntry(HWMUnitTests* suite) {
    uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    suite->hwm.setMacPeer(SerialIdentifier::INPUT_JACK, makeTestPeer(mac, SerialIdentifier::INPUT_JACK));
    ASSERT_NE(suite->hwm.getMacPeer(SerialIdentifier::INPUT_JACK), nullptr);

    suite->hwm.removeMacPeer(SerialIdentifier::INPUT_JACK);
    EXPECT_EQ(suite->hwm.getMacPeer(SerialIdentifier::INPUT_JACK), nullptr);
}
