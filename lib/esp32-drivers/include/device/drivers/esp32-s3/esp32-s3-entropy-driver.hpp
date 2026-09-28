#pragma once

#include "device/drivers/entropy-interface.hpp"

#include <esp_random.h>

/// Hardware RNG. Seeded from RF noise once the radio is up, which ESP-NOW guarantees
/// here, so it owes nothing to the clock or to a stored counter.
class Esp32S3Entropy : public EntropyInterface {
public:
    uint32_t next32() override { return esp_random(); }
};
