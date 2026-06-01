//
// Created by Elli Furedy on 10/9/2024.
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <functional>

using SerialBytesCallback = std::function<void(const uint8_t*, size_t)>;

enum class SerialIdentifier {
    OUTPUT_JACK = 0,
    INPUT_JACK = 1
};

class HWSerialWrapper {
    public:
    virtual ~HWSerialWrapper() = default;
    virtual int availableForWrite() = 0;
    virtual int available() = 0;
    virtual void flush() = 0;
    virtual void setBytesCallback(const SerialBytesCallback& callback) = 0;
    virtual void writeBytes(const uint8_t* data, size_t len) = 0;

    // Electrical-level cable disconnect detection.
    // Returns true when the UART RX pin has been sustained at the "no remote
    // drive" level for long enough to confidently say "cable unplugged" — on
    // ESP32, that's ~50ms of continuous pullup-driven HIGH while the UART is
    // configured invert=true (idle is normally actively LOW). The
    // implementation tracks consecutive-sample state internally; callers can
    // poll once per sync tick without smoothing on their end. Used by RDC as
    // a parallel fast-detection path complementing the protocol-level
    // silent-link (HELLO-gap) jack-dead trigger. Native test stubs return
    // false (no physical cable).
    virtual bool isCableDisconnected() = 0;
};