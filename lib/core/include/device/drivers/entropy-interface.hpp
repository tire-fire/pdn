#pragma once

#include <cstdint>

/// A source of values a caller can treat as fresh, including after the device resets.
///
/// The platform clock is not one, which is the reason this exists: at the moment a device
/// has finished booting it reads much the same either side of a reset, and that is exactly
/// when a fresh value is needed. A counter in RAM is worse — it restarts. Anything that
/// must still be distinct after a device resets takes its value here.
class EntropyInterface {
public:
    virtual ~EntropyInterface() = default;
    /// A fresh 32-bit value. Repeats are possible and their likelihood is the
    /// implementation's to state; whether that likelihood is small enough is the caller's
    /// to judge.
    virtual uint32_t next32() = 0;
};
