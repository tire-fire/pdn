#pragma once

#include "device/drivers/entropy-interface.hpp"

#include <random>

/// Desktop stand-in. std::random_device is a real source here, unlike on ESP32 under
/// newlib where it is deterministic — which is why the firmware does not use it.
class NativeEntropyDriver : public EntropyInterface {
public:
    uint32_t next32() override { return distribution(generator); }

private:
    std::random_device seedSource;
    std::mt19937 generator{seedSource()};
    std::uniform_int_distribution<uint32_t> distribution;
};
