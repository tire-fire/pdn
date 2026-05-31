#pragma once

#include <gtest/gtest.h>
#include "device/peer-graph-codec.hpp"

inline void codecHelloRoundTrip() {
    net::Mac src = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    auto frame = peer_graph::encodeHello(src, 2);  // deviceType FDN=2
    EXPECT_EQ(frame.size(), 12u);  // preamble 2 + opcode 1 + payload 7 + CRC 2
    auto decoded = peer_graph::decodeHello(frame);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->source, src);
    EXPECT_EQ(decoded->deviceType, 2);
}

inline void codecBeaconRoundTrip() {
    BeaconRecord b{{0x01, 0, 0, 0, 0, 0}, {0x02, 0, 0, 0, 0, 0}, {0x03, 0, 0, 0, 0, 0}};
    auto frame = peer_graph::encodeBeacon(b);
    EXPECT_EQ(frame.size(), 23u);  // preamble 2 + opcode 1 + payload 18 + CRC 2
    auto decoded = peer_graph::decodeBeacon(frame);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, b);
}

inline void codecBeaconEmptyPeersRoundTrip() {
    BeaconRecord b{{0x01, 0, 0, 0, 0, 0}, {}, {}};
    auto frame = peer_graph::encodeBeacon(b);
    auto decoded = peer_graph::decodeBeacon(frame);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, b);
}

inline void codecRejectsCorruptedCrc() {
    net::Mac src = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    auto frame = peer_graph::encodeHello(src, 1);
    frame[5] ^= 0xFF;  // flip a payload byte; CRC no longer matches
    EXPECT_FALSE(peer_graph::decodeHello(frame).has_value());
}

inline void codecRejectsWrongOpcode() {
    BeaconRecord b{{0x01, 0, 0, 0, 0, 0}, {}, {}};
    auto frame = peer_graph::encodeBeacon(b);
    // A BEACON frame must not decode as a HELLO and vice versa.
    EXPECT_FALSE(peer_graph::decodeHello(frame).has_value());
    net::Mac src = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    auto hello = peer_graph::encodeHello(src, 1);
    EXPECT_FALSE(peer_graph::decodeBeacon(hello).has_value());
}
