#pragma once

#include "device/drivers/entropy-interface.hpp"

#include <esp_random.h>

/// The SoC's hardware RNG. Continuously fed from RF noise while the radio is enabled, so
/// each call draws afresh rather than running from a seed taken once at boot — which is
/// what makes it usable for a value that must differ either side of a reset. With the
/// radio disabled the IDF does not call it a true random source at all, so the bar above
/// is cleared by radio-on operation; every caller here runs long after radio init.
class Esp32S3Entropy : public EntropyInterface {
public:
    uint32_t next32() override { return esp_random(); }
};
