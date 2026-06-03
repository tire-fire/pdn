#pragma once

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "apps/handshake/handshake.hpp"
#include "device-mock.hpp"
#include "utility-tests.hpp"
#include "utils/simple-timer.hpp"
#include "wireless/crc16.hpp"
#include "wireless/handshake-wireless-manager.hpp"

#include <cstdint>
#include <string>
#include <vector>

class HandshakeAppDemuxerTests : public testing::Test {
public:
    void SetUp() override {
        fakeClock = new FakePlatformClock();
        SimpleTimer::setPlatformClock(fakeClock);
        fakeClock->setTime(1000);

        ::testing::DefaultValue<int>::Set(1);
        wirelessManager.initialize(device.wirelessManager);
        app = new HandshakeApp(SerialIdentifier::INPUT_JACK);
        app->start(device.serialManager);
    }

    void TearDown() override {
        delete app;
        SimpleTimer::setPlatformClock(nullptr);
        delete fakeClock;
    }

    // Build a valid binary frame for a given opcode + payload; appends the CRC-16
    // computed over (opcode + payload). The 0xAA 0x55 preamble is prepended.
    std::vector<uint8_t> buildFrame(uint8_t opcode, const std::vector<uint8_t>& payload) {
        std::vector<uint8_t> crcInput;
        crcInput.push_back(opcode);
        crcInput.insert(crcInput.end(), payload.begin(), payload.end());
        const uint16_t c = crc16(crcInput.data(), crcInput.size());

        std::vector<uint8_t> out;
        out.push_back(0xAA);
        out.push_back(0x55);
        out.push_back(opcode);
        out.insert(out.end(), payload.begin(), payload.end());
        out.push_back(static_cast<uint8_t>((c >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(c & 0xFF));
        return out;
    }

    void feed(const std::vector<uint8_t>& bytes) {
        device.inputJackSerial.bytesCallback(bytes.data(), bytes.size());
    }

    MockDevice device;
    HandshakeWirelessManager wirelessManager;
    HandshakeApp* app = nullptr;
    FakePlatformClock* fakeClock = nullptr;
};

inline void handshakeAppValidBinaryFrameRoutesToFrameHandler(HandshakeAppDemuxerTests* s) {
    int frameCount = 0;
    Frame got;
    s->app->setBinaryFrameHandler([&](const Frame& f) { got = f; ++frameCount; });

    // HELLO (opcode 0x00) carries a 6-byte source MAC + 1-byte deviceType.
    std::vector<uint8_t> payload = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x01};
    auto frame = s->buildFrame(0x00, payload);
    s->feed(frame);

    ASSERT_EQ(frameCount, 1);
    EXPECT_EQ(got.opcode, 0x00);
    ASSERT_EQ(got.payload.size(), 7u);
    EXPECT_EQ(got.payload[0], 0x10);
}

inline void handshakeAppBadCrcDropsFrame(HandshakeAppDemuxerTests* s) {
    int frameCount = 0;
    s->app->setBinaryFrameHandler([&](const Frame&) { ++frameCount; });

    // BEACON (opcode 0x01) with an 18-byte payload.
    std::vector<uint8_t> payload(18, 0x33);
    auto frame = s->buildFrame(0x01, payload);
    // Corrupt the trailing CRC bytes (last 2).
    frame[frame.size() - 2] ^= 0xFF;
    frame[frame.size() - 1] ^= 0xFF;
    s->feed(frame);

    EXPECT_EQ(frameCount, 0);
}

inline void handshakeAppUnknownOpcodeDrops(HandshakeAppDemuxerTests* s) {
    int frameCount = 0;
    s->app->setBinaryFrameHandler([&](const Frame&) { ++frameCount; });

    // 0x99 is not in the opcode table. Parser should reject and resync without
    // calling the handler. Feed a few trailing bytes; none should cause issues.
    const std::vector<uint8_t> bytes = {0xAA, 0x55, 0x99, 0x01, 0x02, 0x03, 0x04};
    s->feed(bytes);

    EXPECT_EQ(frameCount, 0);

    // After resync, a valid frame should still parse cleanly.
    auto valid = s->buildFrame(0x00, std::vector<uint8_t>{1,2,3,4,5,6,1});
    s->feed(valid);
    EXPECT_EQ(frameCount, 1);
}

inline void handshakeAppFragmentedFrameAssembles(HandshakeAppDemuxerTests* s) {
    int frameCount = 0;
    Frame got;
    s->app->setBinaryFrameHandler([&](const Frame& f) { got = f; ++frameCount; });

    // HELLO (opcode 0x00) with a 6-byte MAC + 1-byte deviceType payload.
    std::vector<uint8_t> payload = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x01};
    auto frame = s->buildFrame(0x00, payload);

    // Feed the frame one byte at a time.
    for (uint8_t b : frame) {
        uint8_t single = b;
        s->device.inputJackSerial.bytesCallback(&single, 1);
    }

    ASSERT_EQ(frameCount, 1);
    EXPECT_EQ(got.opcode, 0x00);
    ASSERT_EQ(got.payload.size(), 7u);
    EXPECT_EQ(got.payload[0], 0x01);
    EXPECT_EQ(got.payload[5], 0x06);
}

inline void handshakeAppMidFrameTimeoutResets(HandshakeAppDemuxerTests* s) {
    int frameCount = 0;
    s->app->setBinaryFrameHandler([&](const Frame&) { ++frameCount; });

    // Inject just the preamble + opcode + partial payload, then stall.
    // BEACON opcode 0x01 expects 18 payload bytes; feed only 3.
    std::vector<uint8_t> partial = {0xAA, 0x55, 0x01, 0xDE, 0xAD, 0xBE};
    s->feed(partial);
    EXPECT_EQ(frameCount, 0);

    // Advance the clock past the 50ms timeout. The demuxer checks the mid-frame
    // timeout at the start of the next feed(), so the partial frame is cleared
    // before the fresh frame below is consumed.
    s->fakeClock->advance(60);

    // Feed a fresh valid frame; the parser should have reset and pick it up.
    auto valid = s->buildFrame(0x00, std::vector<uint8_t>{1,2,3,4,5,6,1});
    s->feed(valid);
    EXPECT_EQ(frameCount, 1);
}

inline void handshakeAppGarbageThenValidFrameResyncs(HandshakeAppDemuxerTests* s) {
    int frameCount = 0;
    Frame got;
    s->app->setBinaryFrameHandler([&](const Frame& f) { got = f; ++frameCount; });

    // Leading line noise with no preamble must be discarded, and the parser must
    // resync on the next 0xAA 0x55 rather than swallowing the following frame.
    s->feed({0x12, 0x34, 0xFF, 0x00, 0x55});
    auto valid = s->buildFrame(0x00, std::vector<uint8_t>{0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x01});
    s->feed(valid);

    ASSERT_EQ(frameCount, 1);
    EXPECT_EQ(got.opcode, 0x00);
    EXPECT_EQ(got.payload[0], 0xA1);
}

inline void handshakeAppDoubleAAResyncsToPreamble(HandshakeAppDemuxerTests* s) {
    int frameCount = 0;
    s->app->setBinaryFrameHandler([&](const Frame&) { ++frameCount; });

    // A stray extra 0xAA before the real preamble: GotAA on another 0xAA must
    // stay in GotAA so 0xAA 0xAA 0x55 still opens a frame rather than aborting.
    auto valid = s->buildFrame(0x00, std::vector<uint8_t>{1,2,3,4,5,6,1});
    std::vector<uint8_t> withExtraAA;
    withExtraAA.push_back(0xAA);
    withExtraAA.insert(withExtraAA.end(), valid.begin(), valid.end());
    s->feed(withExtraAA);

    EXPECT_EQ(frameCount, 1);
}

inline void handshakeAppSplitPreambleAcrossFeeds(HandshakeAppDemuxerTests* s) {
    int frameCount = 0;
    s->app->setBinaryFrameHandler([&](const Frame&) { ++frameCount; });

    // The 0xAA and 0x55 of the preamble arrive in separate feed() calls; the
    // GotAA state must survive across reads.
    auto valid = s->buildFrame(0x00, std::vector<uint8_t>{1,2,3,4,5,6,1});
    s->feed({valid[0]});                                       // 0xAA
    s->feed(std::vector<uint8_t>(valid.begin() + 1, valid.end())); // 0x55 + body

    EXPECT_EQ(frameCount, 1);
}
