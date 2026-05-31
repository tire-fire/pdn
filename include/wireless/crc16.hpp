#pragma once
#include <cstdint>
#include <cstddef>

// CRC-16/CCITT XMODEM: poly 0x1021, init 0x0000, no reflection, no xorout.
// Validates binary frames over the serial UART link.
inline uint16_t crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0x0000;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int j = 0; j < 8; ++j) {
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
        }
    }
    return crc;
}
