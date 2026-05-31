#pragma once

#include "device/serial-manager.hpp"
#include "device/serial-frame-demuxer.hpp"

class HandshakeApp {
public:
    explicit HandshakeApp(SerialIdentifier jack);

    // Install the byte demuxer's raw-bytes callback on the serial manager for
    // this jack. The demuxer runs on the UART event task via this callback.
    void start(SerialManager* serialManager);

    // Defer the demuxer reset to the UART event task: clearing the parser's
    // buffers synchronously would race feed() on the event task.
    void resetDemuxer() { demuxer_.requestReset(); }

    // Register a handler for validated binary roster frames. Bound by RDC
    // (or by tests). Forwarded to the byte demuxer.
    void setBinaryFrameHandler(BinaryFrameHandler cb) { demuxer_.setBinaryFrameHandler(std::move(cb)); }

    // Field-telemetry hooks for the byte-level parser; RDC binds these to its
    // RosterStats counters, tests can bind to inspect. Both default to no-op.
    void setCrcFailHandler(SerialFrameDemuxer::ParserEventHandler cb) { demuxer_.setCrcFailHandler(std::move(cb)); }
    void setParserResyncHandler(SerialFrameDemuxer::ParserEventHandler cb) { demuxer_.setParserResyncHandler(std::move(cb)); }

private:
    SerialIdentifier jack_;

    // Owns the byte-stream parser. Fed on the UART event task via the serial
    // wrapper's bytes callback; requestReset() is called from the main loop on
    // jack teardown.
    SerialFrameDemuxer demuxer_;
};
