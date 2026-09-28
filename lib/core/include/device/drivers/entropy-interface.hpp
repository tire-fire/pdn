#pragma once

#include <cstdint>

/// A source of values that do not repeat across power cycles.
///
/// The platform clock is not one, which is the reason this exists: at the moment a
/// device has finished booting it reads much the same either side of a reset, and that
/// is exactly when a fresh value is needed. A counter in RAM is worse — it restarts.
/// Anything that must still be distinct after a device resets takes its value here.
class EntropyInterface {
public:
    virtual ~EntropyInterface() = default;
    /// A fresh value. Repeats are permitted but must be rare enough that two of them
    /// landing on the same pair of live tournaments is not worth designing against.
    virtual uint32_t next32() = 0;
};
