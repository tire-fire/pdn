#pragma once

#include <array>
#include <cstddef>
#include <cstring>
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "wireless/reliable-channel.hpp"
#include "wireless/wireless-transport.hpp"
#include "device/wireless-manager.hpp"
#include "device-mock.hpp"

// Generic wire payload for exercising the transport. The reliable layer only
// requires a settable `seqId` member; the rest is opaque bytes. Defined here so
// these tests stay independent of any game-specific payload type.
struct TransportTestPayload {
    uint8_t tag;
    uint8_t seqId;
    uint8_t data[14];
} __attribute__((packed));

// Probe subclass exposing protected nextSeqId for testing.
// Provides a no-op deliverBytes so the (otherwise pure-virtual) base
// becomes instantiable.
class ProbeChannel : public ReliableChannelBase {
public:
    using ReliableChannelBase::ReliableChannelBase;
    using ReliableChannelBase::nextSeqId;
    bool deliverBytes(const uint8_t*, const uint8_t*, size_t) override { return false; }
};

TEST(ReliableChannelBaseTest, nextSeqIdWrapsAfter255) {
    Resender resender(nullptr);
    ProbeChannel ch(nullptr, &resender, PktType::kChainGameEvent, 0,
                    [](uint8_t, const uint8_t*){});
    for (int i = 1; i <= 255; ++i) {
        ASSERT_EQ(ch.nextSeqId(), static_cast<uint8_t>(i));
    }
    // The 256th call wraps back to 1 (zero is reserved for "no ack expected").
    ASSERT_EQ(ch.nextSeqId(), uint8_t{1});
}

TEST(WirelessTransportTest, channelsClaimDistinctPktTypes) {
    ::testing::NiceMock<MockPeerComms> mockComms;
    WirelessManager wm(&mockComms, nullptr);
    WirelessTransport transport(&wm);
    auto* ch = transport.channel<TransportTestPayload>(
        PktType::kChainGameEvent,
        [](uint8_t, const uint8_t*){});
    ASSERT_NE(ch, nullptr);
    ASSERT_DEATH({
        transport.channel<TransportTestPayload>(
            PktType::kChainGameEvent,
            [](uint8_t, const uint8_t*){});
    }, "duplicate channel claim");
}

TEST(WirelessTransportTest, distinctSubTypesUnderSamePktType) {
    ::testing::NiceMock<MockPeerComms> mockComms;
    WirelessManager wm(&mockComms, nullptr);
    WirelessTransport transport(&wm);
    enum class TestCmd : uint8_t { A = 0, B = 1 };
    auto* chA = transport.channel<TransportTestPayload, TestCmd>(
        PktType::kShootoutCommand, TestCmd::A,
        [](uint8_t, const uint8_t*){});
    auto* chB = transport.channel<TransportTestPayload, TestCmd>(
        PktType::kShootoutCommand, TestCmd::B,
        [](uint8_t, const uint8_t*){});
    ASSERT_NE(chA, nullptr);
    ASSERT_NE(chB, nullptr);
    ASSERT_NE(chA, chB);
}

TEST(WirelessTransportTest, deliverIncomingReturnsFalseWhenChannelMissing) {
    ::testing::NiceMock<MockPeerComms> mockComms;
    WirelessManager wm(&mockComms, nullptr);
    WirelessTransport transport(&wm);
    uint8_t from[6] = {1,2,3,4,5,6};
    uint8_t data[4] = {0};
    EXPECT_FALSE(transport.deliverIncoming(
        PktType::kChainGameEvent, 0, from, data, sizeof(data)));
}

TEST(WirelessTransportTest, sendReliableTriggersAck) {
    ::testing::NiceMock<MockPeerComms> mockComms;
    WirelessManager wm(&mockComms, nullptr);
    WirelessTransport transport(&wm);

    bool abandoned = false;
    auto* ch = transport.channel<TransportTestPayload>(
        PktType::kChainGameEvent,
        [&](uint8_t, const uint8_t*){ abandoned = true; });

    bool received = false;
    TransportTestPayload deliveredP{};
    ch->onReceive([&](const uint8_t* /*mac*/, const TransportTestPayload& p){
        received = true;
        deliveredP = p;
    });

    using ::testing::_;
    using ::testing::Return;
    EXPECT_CALL(mockComms, sendData(_, _, _, _))
        .Times(::testing::AnyNumber())
        .WillRepeatedly(Return(1));

    uint8_t target[6] = {1,2,3,4,5,6};
    TransportTestPayload p{};
    p.tag = 7;
    uint8_t seq = ch->sendReliable(target, p);
    ASSERT_NE(seq, 0);
    ASSERT_TRUE(ch->isPending(target));

    // Synthesize an ack from target.
    AckPayload ack{ static_cast<uint8_t>(PktType::kChainGameEvent), 0, seq };
    transport.onAckPacket(target, reinterpret_cast<const uint8_t*>(&ack), sizeof(ack));
    ASSERT_FALSE(ch->isPending(target));
    ASSERT_FALSE(abandoned);

    // Inbound packet: deliverIncoming -> channel.deliver -> onReceive fires.
    TransportTestPayload incoming{};
    incoming.tag = 99;
    incoming.seqId = 42;
    transport.deliverIncoming(PktType::kChainGameEvent, 0,
                              target,
                              reinterpret_cast<const uint8_t*>(&incoming),
                              sizeof(incoming));
    ASSERT_TRUE(received);
    ASSERT_EQ(deliveredP.tag, 99);
}

TEST(WirelessTransportTest, abandonAfterMaxRetries) {
    ::testing::NiceMock<MockPeerComms> mockComms;
    WirelessManager wm(&mockComms, nullptr);
    WirelessTransport transport(&wm);

    int abandonCount = 0;
    uint8_t abandonedSeqId = 0;
    std::array<uint8_t, 6> abandonedMac{};
    auto* ch = transport.channel<TransportTestPayload>(
        PktType::kChainGameEvent,
        [&](uint8_t seqId, const uint8_t* mac){
            abandonCount++;
            abandonedSeqId = seqId;
            std::memcpy(abandonedMac.data(), mac, 6);
        });

    using ::testing::_;
    using ::testing::Return;
    EXPECT_CALL(mockComms, sendData(_, _, _, _))
        .Times(::testing::AnyNumber())
        .WillRepeatedly(Return(1));

    uint8_t target[6] = {1,2,3,4,5,6};
    TransportTestPayload p{};
    p.tag = 7;
    uint8_t seq = ch->sendReliable(target, p);

    FakePlatformClock clock;
    SimpleTimer::setPlatformClock(&clock);
    for (int i = 0; i < 8; ++i) {
        clock.advance(1000);
        transport.sync();
    }
    SimpleTimer::setPlatformClock(nullptr);

    ASSERT_EQ(abandonCount, 1);
    ASSERT_EQ(abandonedSeqId, seq);
    ASSERT_EQ(std::memcmp(abandonedMac.data(), target, 6), 0);
    ASSERT_FALSE(ch->isPending(target));
}

TEST(WirelessTransportTest, ackRoutesByChannelSubType) {
    // Two channels sharing PktType with distinct subTypes allocate seqIds
    // independently and can have pending sends with identical
    // (type, target, seqId). An ack routed to one channel must clear that
    // channel's entry, not the other's — otherwise the unacked channel
    // retries to abandon and tears down state via its abandon callback.
    ::testing::NiceMock<MockPeerComms> mockComms;
    WirelessManager wm(&mockComms, nullptr);
    WirelessTransport transport(&wm);

    enum class TestCmd : uint8_t { A = 0, B = 1 };
    auto* chA = transport.channel<TransportTestPayload, TestCmd>(
        PktType::kShootoutCommand, TestCmd::A,
        [](uint8_t, const uint8_t*){});
    auto* chB = transport.channel<TransportTestPayload, TestCmd>(
        PktType::kShootoutCommand, TestCmd::B,
        [](uint8_t, const uint8_t*){});

    using ::testing::_;
    using ::testing::Return;
    EXPECT_CALL(mockComms, sendData(_, _, _, _))
        .Times(::testing::AnyNumber())
        .WillRepeatedly(Return(1));

    uint8_t target[6] = {1,2,3,4,5,6};
    TransportTestPayload p{};
    // Send B first so its pending entry is inserted ahead of A's in the
    // Resender vector. The buggy onAck (matching only on type+target+seqId)
    // would erase B when A is acked.
    uint8_t seqB = chB->sendReliable(target, p);
    uint8_t seqA = chA->sendReliable(target, p);
    ASSERT_EQ(seqA, seqB);
    ASSERT_TRUE(chA->isPending(target));
    ASSERT_TRUE(chB->isPending(target));

    AckPayload ack{
        static_cast<uint8_t>(PktType::kShootoutCommand),
        static_cast<uint8_t>(TestCmd::A),
        seqA
    };
    transport.onAckPacket(target, reinterpret_cast<const uint8_t*>(&ack), sizeof(ack));

    ASSERT_FALSE(chA->isPending(target));
    ASSERT_TRUE(chB->isPending(target));
}

TEST(WireFormatTest, shootoutConfirmPayloadSize) {
    static_assert(sizeof(ShootoutConfirmPayload) == 2 + 6 + kNameLength,
                  "ShootoutConfirmPayload layout drift");
    SUCCEED();
}
TEST(WireFormatTest, shootoutMatchStartPayloadSize) {
    static_assert(sizeof(ShootoutMatchStartPayload) == 2 + 6 + 6 + 1,
                  "ShootoutMatchStartPayload layout drift");
    SUCCEED();
}
TEST(WireFormatTest, shootoutMatchResultPayloadSize) {
    static_assert(sizeof(ShootoutMatchResultPayload) == 2 + 6 + 6 + 1,
                  "ShootoutMatchResultPayload layout drift");
    SUCCEED();
}
TEST(WireFormatTest, bracketEntryCarriesBatchHeader) {
    static_assert(sizeof(ShootoutBracketEntryPayload) == 1 + 1 + 1 + 1 + 1 + 6,
                  "ShootoutBracketEntryPayload size drift");
    static_assert(offsetof(ShootoutBracketEntryPayload, seqId) == 1, "");
    static_assert(offsetof(ShootoutBracketEntryPayload, batchId) == 2, "");
    static_assert(offsetof(ShootoutBracketEntryPayload, slot) == 3, "");
    static_assert(offsetof(ShootoutBracketEntryPayload, totalSlots) == 4, "");
    static_assert(offsetof(ShootoutBracketEntryPayload, mac) == 5, "");
    SUCCEED();
}
TEST(WireFormatTest, shootoutTournamentEndPayloadSize) {
    static_assert(sizeof(ShootoutTournamentEndPayload) == 2 + 6,
                  "ShootoutTournamentEndPayload layout drift");
    SUCCEED();
}
TEST(WireFormatTest, shootoutPeerLostPayloadSize) {
    static_assert(sizeof(ShootoutPeerLostPayload) == 2 + 6,
                  "ShootoutPeerLostPayload layout drift");
    SUCCEED();
}
TEST(WireFormatTest, shootoutAbortPayloadSize) {
    static_assert(sizeof(ShootoutAbortPayload) == 2,
                  "ShootoutAbortPayload layout drift");
    SUCCEED();
}
