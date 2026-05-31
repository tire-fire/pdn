#include "device/serial-frame-demuxer.hpp"

#include "utils/simple-timer.hpp"
#include "wireless/crc16.hpp"

#include <limits>

unsigned long SerialFrameDemuxer::nowMs() {
    auto* clk = SimpleTimer::getPlatformClock();
    if (clk == nullptr) {
        // A null platform clock breaks timeout semantics — silently returning 0
        // would make the parser timeout never fire on a misconfigured boot or
        // test setup. Return max() so the next (now - lastByteMs_) subtraction
        // overflows large and forces an immediate timeout, surfacing the
        // misconfiguration loudly.
        return std::numeric_limits<unsigned long>::max();
    }
    return clk->milliseconds();
}

size_t SerialFrameDemuxer::opcodePayloadLen(uint8_t opcode) {
    switch (opcode) {
        case 0x00: return 7;   // HELLO: source mac[6] + deviceType[1]
        case 0x01: return 18;  // BEACON: source[6] + inPeer[6] + outPeer[6]
        default:   return 0;
    }
}

void SerialFrameDemuxer::resetParser() {
    parser_.state = ParseState::ScanForSync;
    parser_.opcode = 0;
    parser_.payloadBuf.clear();
    parser_.expectedPayloadLen = 0;
    parser_.crcBytes[0] = 0;
    parser_.crcBytes[1] = 0;
    parser_.crcBytesRead = 0;
}

void SerialFrameDemuxer::checkTimeout() {
    if (parser_.state == ParseState::ScanForSync) return;
    const unsigned long now = nowMs();
    if (now - lastByteMs_ > kParserTimeoutMs) {
        resetParser();
        if (parserResyncHandler_) parserResyncHandler_();
    }
}

void SerialFrameDemuxer::feed(const uint8_t* data, size_t len) {
    // Honor a reset requested from another thread before consuming this burst,
    // so the only thread that ever clears the parser is this one. Then check the
    // mid-frame timeout — also before consuming — so a stale partial frame is
    // cleared before a late byte completes a corrupt one.
    if (resetRequested_.exchange(false)) {
        resetParser();
    }
    checkTimeout();
    const unsigned long now = nowMs();
    lastByteMs_ = now;
    for (size_t i = 0; i < len; ++i) {
        const uint8_t b = data[i];
        switch (parser_.state) {
            case ParseState::ScanForSync:
                if (b == 0xAA) {
                    parser_.state = ParseState::GotAA;
                }
                break;
            case ParseState::GotAA:
                if (b == 0x55) {
                    parser_.state = ParseState::ReadingOpcode;
                } else if (b == 0xAA) {
                    // Stay in GotAA: a stray 0xAA followed by the real preamble (0xAA 0x55) should
                    // still detect the frame start. Going back to ScanForSync would drop the second
                    // 0xAA, requiring a 3-byte preamble before re-syncing.
                } else {
                    resetParser();
                }
                break;
            case ParseState::ReadingOpcode:
                if (b > 0x01) {  // only HELLO (0x00) and BEACON (0x01) exist
                    resetParser();
                    break;
                }
                parser_.opcode = b;
                parser_.expectedPayloadLen = opcodePayloadLen(b);
                parser_.payloadBuf.clear();
                if (parser_.expectedPayloadLen == 0) {
                    parser_.state = ParseState::ReadingCrc;
                    parser_.crcBytesRead = 0;
                } else {
                    parser_.state = ParseState::ReadingPayload;
                }
                break;
            case ParseState::ReadingPayload:
                parser_.payloadBuf.push_back(b);
                if (parser_.payloadBuf.size() >= parser_.expectedPayloadLen) {
                    parser_.state = ParseState::ReadingCrc;
                    parser_.crcBytesRead = 0;
                }
                break;
            case ParseState::ReadingCrc:
                parser_.crcBytes[parser_.crcBytesRead++] = b;
                if (parser_.crcBytesRead == 2) {
                    const uint16_t got = (static_cast<uint16_t>(parser_.crcBytes[0]) << 8)
                                       | static_cast<uint16_t>(parser_.crcBytes[1]);
                    std::vector<uint8_t> crcInput;
                    crcInput.reserve(1 + parser_.payloadBuf.size());
                    crcInput.push_back(parser_.opcode);
                    crcInput.insert(crcInput.end(),
                                    parser_.payloadBuf.begin(),
                                    parser_.payloadBuf.end());
                    const uint16_t expected = crc16(crcInput.data(), crcInput.size());
                    if (got == expected) {
                        if (binaryFrameHandler_) {
                            Frame f{parser_.opcode, parser_.payloadBuf};
                            binaryFrameHandler_(f);
                        }
                    } else if (crcFailHandler_) {
                        crcFailHandler_();
                    }
                    resetParser();
                }
                break;
        }
    }
}
