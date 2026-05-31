#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// A validated binary frame surfaced to downstream consumers (RDC's ingestSerial,
// bound via setBinaryFrameHandler). opcode 0x00 = HELLO (payload mac[6] +
// deviceType[1]); opcode 0x01 = BEACON (payload source[6] + inPeer[6] +
// outPeer[6]). payload holds the raw bytes between the opcode and the CRC.
struct Frame {
    uint8_t opcode;
    std::vector<uint8_t> payload;
};

using BinaryFrameHandler = std::function<void(const Frame&)>;

// Parses one jack's serial byte stream into framed binary roster frames
// (preamble 0xAA 0x55 + opcode + payload + CRC-16). One instance per jack.
// feed() runs on the UART event task; requestReset() is callable from the main
// loop and is honored at the start of the next feed(), so the parser's
// std::vector state is only ever mutated by the feeding thread.
class SerialFrameDemuxer {
public:
    void feed(const uint8_t* data, size_t len);

    // Request a parser reset from another thread. The actual clear happens at
    // the start of the next feed(), keeping parser state single-owner.
    void requestReset() { resetRequested_.store(true); }

    void setBinaryFrameHandler(BinaryFrameHandler cb) { binaryFrameHandler_ = std::move(cb); }

    // CRC-fail fires when a preamble matches and the framing length is satisfied
    // but the trailing CRC-16 does not. Parser-resync fires when the
    // kParserTimeoutMs mid-frame watchdog clears a partial frame.
    using ParserEventHandler = std::function<void()>;
    void setCrcFailHandler(ParserEventHandler cb) { crcFailHandler_ = std::move(cb); }
    void setParserResyncHandler(ParserEventHandler cb) { parserResyncHandler_ = std::move(cb); }

private:
    enum class ParseState {
        ScanForSync,
        GotAA,
        ReadingOpcode,
        ReadingPayload,
        ReadingCrc,
    };

    struct ParserState {
        ParseState state = ParseState::ScanForSync;
        uint8_t opcode = 0;
        std::vector<uint8_t> payloadBuf;
        size_t expectedPayloadLen = 0;
        uint8_t crcBytes[2] = {0, 0};
        size_t crcBytesRead = 0;
    };

    ParserState parser_;

    // Timestamp of the last arriving byte; checkTimeout() uses it to clear a
    // stalled mid-frame parse.
    unsigned long lastByteMs_ = 0;

    // Set from another thread (the main loop) to ask for a reset; consumed at the
    // top of the next feed() so the demuxer state stays single-owner.
    std::atomic<bool> resetRequested_{false};

    BinaryFrameHandler binaryFrameHandler_;
    ParserEventHandler crcFailHandler_;
    ParserEventHandler parserResyncHandler_;

    // Wall-clock window after which a mid-frame parser resets to scan-for-sync.
    static constexpr unsigned long kParserTimeoutMs = 50;

    void resetParser();
    void checkTimeout();
    static size_t opcodePayloadLen(uint8_t opcode);
    static unsigned long nowMs();
};
