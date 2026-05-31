//
// Created by Elli Furedy on 10/9/2024.
//

#pragma once

#include "device/drivers/driver-interface.hpp"
#include "device/drivers/logger.hpp"
#include <Arduino.h>
#include <esp_rom_gpio.h>
#include <driver/gpio.h>
#include "device/device-constants.hpp"
#include <HardwareSerial.h>
#include <string>

class Esp32s3SerialOut : public SerialDriverInterface {
public:
    explicit Esp32s3SerialOut(const std::string& name) : SerialDriverInterface(name) {}

    ~Esp32s3SerialOut() override {
        bytesCallback = nullptr;
    }

    int initialize() override {

        gpio_reset_pin(GPIO_NUM_38);
        gpio_reset_pin(GPIO_NUM_39);

        esp_rom_gpio_pad_select_gpio(GPIO_NUM_38);
        esp_rom_gpio_pad_select_gpio(GPIO_NUM_39);

        pinMode(TXt, OUTPUT);
        pinMode(TXr, INPUT);

        Serial1.begin(BAUDRATE, SERIAL_8N1, TXr, TXt, true);
        Serial1.setTimeout(100);  // generic UART read timeout

        // Drain RX on the UART event task. onlyOnTimeout (default) fires the
        // callback one symbol-gap after a burst's last byte, so a HELLO's
        // liveness timestamp lands within ~1ms of arrival regardless of
        // main-loop scheduling. That lets the silent-link watchdog hold a tight
        // threshold without false-firing while the main loop is busy.
        Serial1.onReceive([this]() { drainUart(); });

        // RX bias on the OUTPUT jack (TXr = GPIO 38) is a PULLDOWN, not a
        // pullup. This jack receives on the cable TIP — the least reliable TRS
        // contact — and with invert=true the connected-idle level is LOW. A
        // pullup biases toward HIGH (the wrong way), so a marginal/flexing tip
        // contact floats up to a false "disconnected" HIGH and trips the silent
        // link, flapping the link. A pulldown holds the line at the correct
        // idle level through a poor contact; the remote's active HIGH data
        // pulses overpower the weak internal pulldown, so reception is intact.
        // The GPIO float-detector that the bias direction would feed is unused
        // (enableGpioDisconnectDetection_ = false); disconnect is detected by the
        // HELLO silent-link, so the bias only has to keep reception clean.
        gpio_set_pull_mode(static_cast<gpio_num_t>(TXr), GPIO_PULLDOWN_ONLY);
        return 0;
    };

    void exec() override {
        // Sample the RX pin level once per exec. Maintain a saturating
        // up/down score (rises on HIGH, falls on LOW, clamped 0..ceiling).
        // Floating cable averages mostly-HIGH so the score climbs to ceiling
        // within ~50ms (kRxFloatScoreCeiling=500 samples × ~104μs/sample);
        // a connected remote drives the line LOW during idle and the score
        // stays at 0. Latch a disconnect state at ceiling and only clear it
        // when score returns to 0 — prevents thrashing on mechanical bounce
        // when the cable is partially seated.
        const int level = gpio_get_level(static_cast<gpio_num_t>(TXr));
        if (level == 1) {
            if (rxHighScore_ < kRxFloatScoreCeiling) rxHighScore_++;
        } else if (rxHighScore_ > 0) {
            rxHighScore_--;
        }
        if (!rxDisconnected_ && rxHighScore_ == kRxFloatScoreCeiling) {
            rxDisconnected_ = true;
            LOG_W("GPIO_O", "yank detected pin=%d", (int)TXr);
        } else if (rxDisconnected_ && rxHighScore_ == 0) {
            rxDisconnected_ = false;
            LOG_W("GPIO_O", "yank cleared pin=%d", (int)TXr);
        }
    }

    bool isCableDisconnected() override {
        return rxDisconnected_;
    }

    int availableForWrite() override {
        return Serial1.availableForWrite();
    }

    int available() override {
        return Serial1.available();
    }

    void flush() override {
        Serial1.flush();
    }

    void setBytesCallback(const SerialBytesCallback& callback) override {
        bytesCallback = callback;
    }

    void writeBytes(const uint8_t* data, size_t len) override {
        Serial1.write(data, len);
    }

    private:
    SerialBytesCallback bytesCallback;
    // GPIO disconnect detection: count of consecutive exec() ticks where the
    // RX pin was HIGH. exec runs every ~5-10ms; threshold of 10 = ~50-100ms
    // of sustained HIGH before declaring disconnect. Brief data-driven HIGHs
    // (~52μs at 19200 baud) never cross threshold.
    // Disconnect-state machine. The RX pin on an unplugged cable floats
    // mostly HIGH against the internal pullup but picks up enough noise to
    // occasionally read LOW. A strict "N consecutive HIGH samples" check
    // resets on every brief LOW; instead use a saturating up/down counter
    // (rises on HIGH samples, falls on LOW samples, clamped 0..ceiling),
    // and latch a disconnect state at the high threshold + clear it only
    // when the counter drops back to 0. exec() runs at ~10kHz on hardware
    // (~104μs/tick), so a ceiling of 500 ≈ 50ms of "mostly HIGH" accumulation
    // — well past any mechanical bounce, well inside the 300ms perception
    // window for cable yank.
    uint16_t rxHighScore_ = 0;
    bool rxDisconnected_ = false;
    static constexpr uint16_t kRxFloatScoreCeiling = 500;

    // Runs on the UART event task. Drains the RX FIFO and feeds each burst into
    // the binary frame demuxer (bytesCallback). Touches only event-task-owned
    // state (the parser behind bytesCallback).
    void drainUart() {
        uint8_t buf[64];
        while (Serial1.available() > 0) {
            size_t n = 0;
            while (n < sizeof(buf) && Serial1.available() > 0) {
                int b = Serial1.read();
                if (b < 0) break;
                buf[n++] = static_cast<uint8_t>(b);
            }
            if (n == 0) break;
            if (bytesCallback) {
                bytesCallback(buf, n);
            }
        }
    }
};

class Esp32s3SerialIn : public SerialDriverInterface {
public:
    explicit Esp32s3SerialIn(const std::string& name) : SerialDriverInterface(name) {}

    ~Esp32s3SerialIn() override {
        bytesCallback = nullptr;
    }

    int initialize() override {
        gpio_reset_pin(GPIO_NUM_40);
        gpio_reset_pin(GPIO_NUM_41);

        esp_rom_gpio_pad_select_gpio(GPIO_NUM_40);
        esp_rom_gpio_pad_select_gpio(GPIO_NUM_41);

        pinMode(RXt, OUTPUT);
        pinMode(RXr, INPUT);

        Serial2.begin(BAUDRATE, SERIAL_8N1, RXr, RXt, true);
        Serial2.setTimeout(100);  // generic UART read timeout

        // Drain RX on the UART event task — see Esp32s3SerialOut::initialize().
        Serial2.onReceive([this]() { drainUart(); });

        // Same rationale as Esp32s3SerialOut: pullup on the RX pin lets us
        // tell "remote disconnected" (line floats HIGH against the pullup)
        // from "remote idle" (actively driven LOW by the remote in invert=true
        // mode). Sustained HIGH = physical unplug. ~50ms detection budget.
        gpio_pullup_en(static_cast<gpio_num_t>(RXr));
        return 0;
    };

    void exec() override {
        // GPIO disconnect sampling — see Esp32s3SerialOut::exec() for rationale.
        const int level = gpio_get_level(static_cast<gpio_num_t>(RXr));
        if (level == 1) {
            if (rxHighScore_ < kRxFloatScoreCeiling) rxHighScore_++;
        } else if (rxHighScore_ > 0) {
            rxHighScore_--;
        }
        if (!rxDisconnected_ && rxHighScore_ == kRxFloatScoreCeiling) {
            rxDisconnected_ = true;
            LOG_W("GPIO_I", "yank detected pin=%d", (int)RXr);
        } else if (rxDisconnected_ && rxHighScore_ == 0) {
            rxDisconnected_ = false;
            LOG_W("GPIO_I", "yank cleared pin=%d", (int)RXr);
        }
    }

    bool isCableDisconnected() override {
        return rxDisconnected_;
    }

    int availableForWrite() override {
        return Serial2.availableForWrite();
    }

    int available() override {
        return Serial2.available();
    }

    void flush() override {
        Serial2.flush();
    }

    void setBytesCallback(const SerialBytesCallback& callback) override {
        bytesCallback = callback;
    }

    void writeBytes(const uint8_t* data, size_t len) override {
        Serial2.write(data, len);
    }

    private:
    SerialBytesCallback bytesCallback;
    // GPIO disconnect detection: count of consecutive exec() ticks where the
    // RX pin was HIGH. exec runs every ~5-10ms; threshold of 10 = ~50-100ms
    // of sustained HIGH before declaring disconnect. Brief data-driven HIGHs
    // (~52μs at 19200 baud) never cross threshold.
    // Disconnect-state machine. The RX pin on an unplugged cable floats
    // mostly HIGH against the internal pullup but picks up enough noise to
    // occasionally read LOW. A strict "N consecutive HIGH samples" check
    // resets on every brief LOW; instead use a saturating up/down counter
    // (rises on HIGH samples, falls on LOW samples, clamped 0..ceiling),
    // and latch a disconnect state at the high threshold + clear it only
    // when the counter drops back to 0. exec() runs at ~10kHz on hardware
    // (~104μs/tick), so a ceiling of 500 ≈ 50ms of "mostly HIGH" accumulation
    // — well past any mechanical bounce, well inside the 300ms perception
    // window for cable yank.
    uint16_t rxHighScore_ = 0;
    bool rxDisconnected_ = false;
    static constexpr uint16_t kRxFloatScoreCeiling = 500;

    void drainUart() {
        uint8_t buf[64];
        while (Serial2.available() > 0) {
            size_t n = 0;
            while (n < sizeof(buf) && Serial2.available() > 0) {
                int b = Serial2.read();
                if (b < 0) break;
                buf[n++] = static_cast<uint8_t>(b);
            }
            if (n == 0) break;
            if (bytesCallback) {
                bytesCallback(buf, n);
            }
        }
    }
};