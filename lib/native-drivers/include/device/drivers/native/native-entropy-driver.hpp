#pragma once

#include "device/drivers/entropy-interface.hpp"

#include <random>

/// Desktop stand-in. `std::random_device` is a real entropy source in this toolchain, so
/// it seeds a generator once and draws from that. The firmware deliberately does not use
/// it: on the target it can fall back to a deterministic sequence, which is the failure
/// this whole interface exists to avoid. A 32-bit draw from a seeded Mersenne twister
/// clears the bar the interface sets for anything the sim runs.
class NativeEntropy : public EntropyInterface {
public:
    uint32_t next32() override { return distribution(generator); }

private:
    std::mt19937 generator{std::random_device{}()};
    std::uniform_int_distribution<uint32_t> distribution;
};
