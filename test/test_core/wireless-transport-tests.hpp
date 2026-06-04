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
    using ReliableChannelBase::isDuplicateReliableRx;
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

TEST(ReliableChannelBaseTest, rxDedupEvictsOldestSenderWhenFull) {
    // The per-channel RX dedup cursor table is capped (kMaxRxSenders=32). Senders
    // come and go across a session, so when a 33rd distinct sender arrives the
    // oldest cursor is evicted to keep the table bounded. A wrongly-evicted
    // still-active sender just re-seeds on its next packet (one tolerated
    // re-dispatch); a tracked sender keeps deduping.
    Resender resender(nullptr);
    ProbeChannel ch(nullptr, &resender, PktType::kChainGameEvent, 0,
                    [](uint8_t, const uint8_t*){});

    auto sender = [](uint8_t i) {
        return std::array<uint8_t, 6>{0x10, 0x20, 0x30, 0x40, 0x50, i};
    };
    const uint8_t seqId = 7;

    // Fill to the cap: each sender's first packet is fresh, not a duplicate.
    for (uint8_t i = 1; i <= 32; ++i) {
        auto m = sender(i);
        EXPECT_FALSE(ch.isDuplicateReliableRx(m.data(), seqId));
    }
    // The oldest (sender 1) is still tracked: a repeat is deduped.
    auto first = sender(1);
    EXPECT_TRUE(ch.isDuplicateReliableRx(first.data(), seqId));

    // A 33rd distinct sender overflows the table and evicts the oldest cursor.
    auto overflow = sender(33);
    EXPECT_FALSE(ch.isDuplicateReliableRx(overflow.data(), seqId));

    // Sender 1's cursor was evicted, so the same packet now reads as fresh.
    EXPECT_FALSE(ch.isDuplicateReliableRx(first.data(), seqId));

    // A sender that was never evicted still dedupes its repeat.
    auto stillTracked = sender(32);
    EXPECT_TRUE(ch.isDuplicateReliableRx(stillTracked.data(), seqId));
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
    // Send B first so its pending entry sits ahead of A's in the Resender
    // vector. An ack for A must clear only A's slot, never B's, even though
    // they share (type, target, seqId) and differ only by channel sub_type.
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

TEST(ResenderTest, distinctSeqIdsToSameTargetCoexist) {
    // A batch of distinct reliable packets to one peer on one channel (e.g. one
    // BRACKET_ENTRY per bracket slot) must each retain an independent retry
    // slot keyed by seqId; sharing (type, subType, target) must not collapse
    // them into one, or a dropped non-final slot would never retransmit.
    Resender resender(nullptr);  // null wm: transmit() no-ops, retry bookkeeping intact
    uint8_t target[6] = {1,2,3,4,5,6};
    uint8_t payload[4] = {0};
    const auto kStream = Resender::SendMode::KeepDistinct;
    for (uint8_t seq = 1; seq <= 3; ++seq) {
        resender.send(target, PktType::kShootoutCommand, uint8_t{0}, seq,
                      payload, sizeof(payload), kStream);
    }
    EXPECT_EQ(resender.pendingCount(PktType::kShootoutCommand, uint8_t{0}), 3u);

    // Acking the middle seqId clears only that slot.
    EXPECT_TRUE(resender.onAck(PktType::kShootoutCommand, uint8_t{0}, 2, target));
    EXPECT_EQ(resender.pendingCount(PktType::kShootoutCommand, uint8_t{0}), 2u);

    // A genuine re-send of an existing seqId replaces rather than duplicates.
    resender.send(target, PktType::kShootoutCommand, uint8_t{0}, 1,
                  payload, sizeof(payload), kStream);
    EXPECT_EQ(resender.pendingCount(PktType::kShootoutCommand, uint8_t{0}), 2u);

    // cancel() drops every remaining slot to the target on that channel.
    resender.cancel(PktType::kShootoutCommand, uint8_t{0}, target);
    EXPECT_EQ(resender.pendingCount(PktType::kShootoutCommand, uint8_t{0}), 0u);
}

TEST(WirelessTransportTest, droppedNonFinalSlotStillRetransmits) {
    // End-to-end: a coordinator sends three distinct reliable packets to one
    // peer on one channel, the peer acks the later two but the first is lost.
    // The first must remain armed and ultimately abandon, not be silently
    // dropped because two later sends share its (type, subType, target).
    ::testing::NiceMock<MockPeerComms> mockComms;
    WirelessManager wm(&mockComms, nullptr);
    WirelessTransport transport(&wm);

    int abandonCount = 0;
    uint8_t abandonedSeqId = 0;
    auto* ch = transport.channel<TransportTestPayload>(
        PktType::kShootoutCommand,
        [&](uint8_t seqId, const uint8_t*){
            abandonCount++;
            abandonedSeqId = seqId;
        },
        Resender::SendMode::KeepDistinct);  // stream channel, like BRACKET_ENTRY

    using ::testing::_;
    using ::testing::Return;
    EXPECT_CALL(mockComms, sendData(_, _, _, _))
        .Times(::testing::AnyNumber())
        .WillRepeatedly(Return(1));

    uint8_t target[6] = {1,2,3,4,5,6};
    TransportTestPayload p{};
    uint8_t s1 = ch->sendReliable(target, p);  // this one will be "lost"
    uint8_t s2 = ch->sendReliable(target, p);
    uint8_t s3 = ch->sendReliable(target, p);
    ASSERT_NE(s1, s2);
    ASSERT_NE(s2, s3);

    // Peer acks the two later slots; s1 stays pending.
    for (uint8_t seq : {s2, s3}) {
        AckPayload ack{ static_cast<uint8_t>(PktType::kShootoutCommand), 0, seq };
        transport.onAckPacket(target, reinterpret_cast<const uint8_t*>(&ack), sizeof(ack));
    }
    ASSERT_TRUE(ch->isPending(target));  // s1 survived the later sends

    FakePlatformClock clock;
    SimpleTimer::setPlatformClock(&clock);
    for (int i = 0; i < 8; ++i) {
        clock.advance(1000);
        transport.sync();
    }
    SimpleTimer::setPlatformClock(nullptr);

    EXPECT_EQ(abandonCount, 1);
    EXPECT_EQ(abandonedSeqId, s1);
    EXPECT_FALSE(ch->isPending(target));
}

TEST(WirelessTransportTest, duplicateReliableDeliveryDispatchesOnce) {
    // A lost ack makes the sender resend, so the receiver sees the same
    // (sender, seqId) twice. The reliable layer must ack both (to silence the
    // sender) yet dispatch onReceive only once.
    ::testing::NiceMock<MockPeerComms> mockComms;
    WirelessManager wm(&mockComms, nullptr);
    WirelessTransport transport(&wm);

    int deliveries = 0;
    auto* ch = transport.channel<TransportTestPayload>(
        PktType::kChainGameEvent,
        [](uint8_t, const uint8_t*){});
    ch->onReceive([&](const uint8_t*, const TransportTestPayload&){ deliveries++; });

    using ::testing::_;
    using ::testing::Return;
    EXPECT_CALL(mockComms, sendData(_, _, _, _))
        .Times(::testing::AnyNumber())
        .WillRepeatedly(Return(1));

    uint8_t from[6] = {1,2,3,4,5,6};
    TransportTestPayload p{};
    p.seqId = 7;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&p);
    transport.deliverIncoming(PktType::kChainGameEvent, 0, from, bytes, sizeof(p));
    transport.deliverIncoming(PktType::kChainGameEvent, 0, from, bytes, sizeof(p));
    EXPECT_EQ(deliveries, 1);

    // A new seqId from the same sender is a fresh message and dispatches.
    p.seqId = 8;
    transport.deliverIncoming(PktType::kChainGameEvent, 0, from, bytes, sizeof(p));
    EXPECT_EQ(deliveries, 2);

    // A different sender with a colliding seqId must not be suppressed.
    uint8_t from2[6] = {9,9,9,9,9,9};
    p.seqId = 7;
    transport.deliverIncoming(PktType::kChainGameEvent, 0, from2, bytes, sizeof(p));
    EXPECT_EQ(deliveries, 3);
}

TEST(WirelessTransportTest, unsequencedDeliveryNeverDeduped) {
    // seqId==0 is the sendOnce/unsequenced sentinel: those payloads dedup by
    // domain identity at the caller (e.g. forwarded CONFIRMs all carry 0), so
    // the base must dispatch every one.
    ::testing::NiceMock<MockPeerComms> mockComms;
    WirelessManager wm(&mockComms, nullptr);
    WirelessTransport transport(&wm);

    int deliveries = 0;
    auto* ch = transport.channel<TransportTestPayload>(
        PktType::kChainGameEvent,
        [](uint8_t, const uint8_t*){});
    ch->onReceive([&](const uint8_t*, const TransportTestPayload&){ deliveries++; });

    using ::testing::_;
    using ::testing::Return;
    EXPECT_CALL(mockComms, sendData(_, _, _, _))
        .Times(::testing::AnyNumber())
        .WillRepeatedly(Return(1));

    uint8_t from[6] = {1,2,3,4,5,6};
    TransportTestPayload p{};
    p.seqId = 0;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&p);
    transport.deliverIncoming(PktType::kChainGameEvent, 0, from, bytes, sizeof(p));
    transport.deliverIncoming(PktType::kChainGameEvent, 0, from, bytes, sizeof(p));
    EXPECT_EQ(deliveries, 2);
}

