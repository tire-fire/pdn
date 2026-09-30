#pragma once

#include "device/drivers/entropy-interface.hpp"

#include <random>

/// Desktop stand-in. `std::random_device` is a real entropy source in this toolchain, so
/// it is drawn from directly: a generator seeded once from it would add state without
/// adding unpredictability, and this is called once per tournament. The firmware
/// deliberately does not use it — on the target it can fall back to a deterministic
/// sequence, which is the failure this whole interface exists to avoid.
class NativeEntropy : public EntropyInterface {
public:
    uint32_t next32() override { return source(); }

private:
    std::random_device source;
};
