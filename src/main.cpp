#include <Arduino.h>

#include <WiFi.h>
#include <FastLED.h>
#include <Preferences.h>

#include "device/drivers/esp32-s3/esp32-s3-logger-driver.hpp"
#include "device/drivers/esp32-s3/esp32-s3-clock-driver.hpp"
#include "device/drivers/esp32-s3/esp32-s3-1-button-driver.hpp"
#include "device/drivers/esp32-s3/ws2812b-fastled-driver.hpp"
#include "device/drivers/esp32-s3/esp32-s3-haptics-driver.hpp"
#include "device/drivers/esp32-s3/esp32-s3-serial-driver.hpp"
#include "device/drivers/esp32-s3/esp32-s3-http-client-driver.hpp"
#include "device/drivers/esp32-s3/esp-now-driver.hpp"
#include "device/drivers/esp32-s3/ssd1306-u8g2-driver.hpp"
#include "device/drivers/esp32-s3/esp32-s3-prefs-driver.hpp"

#include "utils/simple-timer.hpp"
#include "game/player.hpp"
#include "state/state-machine.hpp"
#include "device/pdn.hpp"
#include "device/device-constants.hpp"
#include "game/quickdraw.hpp"
#include "id-generator.hpp"
#include "wireless/remote-player-manager.hpp"
#include "game/match-manager.hpp"
#include "wireless/wireless-types.hpp"
#include "wireless/quickdraw-wireless-manager.hpp"
#include "wireless/remote-debug-manager.hpp"
#include "wireless/symbol-wireless-manager.hpp"
#include "device/drivers/peer-comms-interface.hpp"
#include "game/quickdraw-resources.hpp"
#include "wireless/resender.hpp"
#include "wireless/wireless-transport.hpp"
#include "device/remote-device-coordinator.hpp"

// WiFi configuration - injected at compile time from wifi_credentials.ini
// See wifi_credentials.ini.example for template
#ifndef WIFI_SSID
#error "WIFI_SSID not defined. Please create wifi_credentials.ini from wifi_credentials.ini.example"
#endif
#ifndef WIFI_PASSWORD
#error "WIFI_PASSWORD not defined. Please create wifi_credentials.ini from wifi_credentials.ini.example"
#endif
#ifndef BASE_URL
#error "BASE_URL not defined. Please create wifi_credentials.ini from wifi_credentials.ini.example"
#endif

WifiConfig* wifiConfig = nullptr;


// ESP32-s3 Drivers (declare as pointers, construct in setup())
Esp32S3Clock* clockDriver = nullptr;
SSD1306U8G2Driver* displayDriver = nullptr;
Esp32S31ButtonDriver* primaryButtonDriver = nullptr;
Esp32S31ButtonDriver* secondaryButtonDriver = nullptr;
WS2812BFastLEDDriver* lightDriver = nullptr;
Esp32S3HapticsDriver* hapticsDriver = nullptr;
Esp32s3SerialOut* serialOutDriver = nullptr;
Esp32s3SerialIn* serialInDriver = nullptr;
Esp32S3HttpClient* httpClientDriver = nullptr;
EspNowManager* peerCommsDriver = nullptr;
Esp32S3Logger* loggerDriver = nullptr;
Esp32S3PrefsDriver* storageDriver = nullptr;

// Core game objects (declare as pointers, construct in setup())
Device* pdn = nullptr;
Player* player = nullptr;

// Game instance
Quickdraw* game = nullptr;

// Remote player management
QuickdrawWirelessManager* quickdrawWirelessManager = nullptr;
SymbolWirelessManager* symbolWirelessManager = nullptr;
RemoteDebugManager* remoteDebugManager = nullptr;
WirelessTransport* gWirelessTransport = nullptr;

void setupEspNow(
    QuickdrawWirelessManager* quickdrawWirelessManager,
    RemoteDebugManager* remoteDebugManager,
    SymbolWirelessManager* symbolWirelessManager,
    PeerCommsInterface* peerCommsDriver) {
    // Register packet handlers
    peerCommsDriver->setPacketHandler(
        PktType::kQuickdrawCommand,
        [](const uint8_t* src, const uint8_t* data, const size_t len, void* userArg) {
            ((QuickdrawWirelessManager*)userArg)->processQuickdrawCommand(src, data, len);
        },
        quickdrawWirelessManager
    );

    // Unified ack: WirelessTransport dispatches acks to the channel that owns the seqId.
    peerCommsDriver->setPacketHandler(
        PktType::kAck,
        [](const uint8_t* src, const uint8_t* data, const size_t len, void* ctx) {
            static_cast<WirelessTransport*>(ctx)->onAckPacket(src, data, len);
        },
        gWirelessTransport
    );
    
    peerCommsDriver->setPacketHandler(
        PktType::kDebugPacket,
        [](const uint8_t* srcAddr, const uint8_t* data, const size_t len, void* userArg) {
            ((RemoteDebugManager*)userArg)->ProcessDebugPacket(srcAddr, data, len);
        },
        remoteDebugManager
    );

    peerCommsDriver->setPacketHandler(
        PktType::kSymbolMatchCommand,
        [](const uint8_t* srcAddr, const uint8_t* data, const size_t len, void* userArg) {
            ((SymbolWirelessManager*)userArg)->processSymbolMatchCommand(srcAddr, data, len);
        },
        symbolWirelessManager
    );

}

// Drives connectivity (HELLO emit + silent-link watchdog) on a fixed 20ms
// cadence independent of the main loop. The main loop's jitter (HTTP, display
// render, ESP-NOW bursts) could otherwise delay the watchdog past its threshold
// and false-fire a disconnect, or starve HELLO emission so a peer thinks WE
// vanished. The receive side (HELLO/BEACON parsing) runs on the UART event
// tasks; this task and those feed RDC's lock-guarded recv queue, which the main
// loop drains in rdc->sync().
static void connectivityTask(void* arg) {
    auto* rdc = static_cast<RemoteDeviceCoordinator*>(arg);
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        rdc->serviceConnectivity(millis());
        vTaskDelayUntil(&last, pdMS_TO_TICKS(20));
    }
}

void setup() {
    Serial.begin(115200);
    while (!Serial) delay(100);

    // Construct drivers FIRST (before anything that might use logging or timers)
    loggerDriver = new Esp32S3Logger(LOGGER_DRIVER_NAME);
    clockDriver = new Esp32S3Clock(PLATFORM_CLOCK_DRIVER_NAME);
    
    // Initialize platform abstractions immediately after constructing them
    g_logger = loggerDriver;
    SimpleTimer::setPlatformClock(clockDriver);
    esp_log_level_set("*", ESP_LOG_VERBOSE);

    // Now construct remaining drivers (safe to use logging and timers now)
    displayDriver = new SSD1306U8G2Driver(DISPLAY_DRIVER_NAME);
    primaryButtonDriver = new Esp32S31ButtonDriver(PRIMARY_BUTTON_DRIVER_NAME, primaryButtonPin);
    secondaryButtonDriver = new Esp32S31ButtonDriver(SECONDARY_BUTTON_DRIVER_NAME, secondaryButtonPin);
    lightDriver = new WS2812BFastLEDDriver(LIGHT_DRIVER_NAME);
    hapticsDriver = new Esp32S3HapticsDriver(HAPTICS_DRIVER_NAME, motorPin);
    serialOutDriver = new Esp32s3SerialOut(SERIAL_OUT_DRIVER_NAME);
    serialInDriver = new Esp32s3SerialIn(SERIAL_IN_DRIVER_NAME);
    
    // WiFi credentials are compile-time constants from build flags
    wifiConfig = new WifiConfig(WIFI_SSID, WIFI_PASSWORD, BASE_URL);
    peerCommsDriver = EspNowManager::CreateEspNowManager(PEER_COMMS_DRIVER_NAME);
    httpClientDriver = new Esp32S3HttpClient(HTTP_CLIENT_DRIVER_NAME, wifiConfig);
    storageDriver = new Esp32S3PrefsDriver(STORAGE_DRIVER_NAME, PREF_NAMESPACE);

    // Create driver configuration
    DriverConfig pdnConfig = {
        {DISPLAY_DRIVER_NAME, displayDriver},
        {PRIMARY_BUTTON_DRIVER_NAME, primaryButtonDriver},
        {SECONDARY_BUTTON_DRIVER_NAME, secondaryButtonDriver},
        {LIGHT_DRIVER_NAME, lightDriver},
        {HAPTICS_DRIVER_NAME, hapticsDriver},
        {SERIAL_OUT_DRIVER_NAME, serialOutDriver},
        {SERIAL_IN_DRIVER_NAME, serialInDriver},
        {HTTP_CLIENT_DRIVER_NAME, httpClientDriver},
        {PEER_COMMS_DRIVER_NAME, peerCommsDriver},
        {PLATFORM_CLOCK_DRIVER_NAME, clockDriver},
        {LOGGER_DRIVER_NAME, loggerDriver},
        {STORAGE_DRIVER_NAME, storageDriver},
    };

    // Create core game objects
    pdn = PDN::createPDN(pdnConfig);
    
    IdGenerator::initialize(clockDriver->milliseconds());
    player = new Player();
    player->setUserID(IdGenerator::getInstance().generateId());

#ifdef AUTO_REGISTER
    // Skip manual onboarding so hardware test runs don't need 4 button-press
    // sessions per reflash. FetchUserDataState recognizes TEST_HUNTER_ID and
    // TEST_BOUNTY_ID and auto-sets role + name without HTTP, falling straight
    // through WelcomeMessage (which has its own timeout transition) into
    // Quickdraw. Role split derives from mac[4] parity — empirically this
    // gives a balanced 2-2 split across the hardware in the lab, while
    // mac[5] parity collapsed all four to HUNTER. Deterministic per device.
    {
        uint8_t mac[6] = {};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        bool isBounty = (mac[4] & 1) != 0;
        std::string autoId = isBounty ? TEST_BOUNTY_ID : TEST_HUNTER_ID;
        std::vector<char> buf(autoId.begin(), autoId.end());
        buf.push_back('\0');
        player->setUserID(buf.data());
        LOG_W("SETUP", "AUTO_REGISTER mac=%02X:%02X role=%s",
              mac[4], mac[5], isBounty ? "BOUNTY" : "HUNTER");
    }
#endif

    pdn->begin();
    // Create wireless managers
    LOG_I("SETUP", "Creating QuickdrawWirelessManager...");
    quickdrawWirelessManager = new QuickdrawWirelessManager();
    LOG_I("SETUP", "Creating SymbolWirelessManager...");
    symbolWirelessManager = new SymbolWirelessManager();
    LOG_I("SETUP", "Creating RemoteDebugManager...");
    remoteDebugManager = new RemoteDebugManager(peerCommsDriver);
    
    // WiFi credentials are compile-time constants from build flags
    remoteDebugManager->Initialize(WIFI_SSID, WIFI_PASSWORD, BASE_URL);

    gWirelessTransport = new WirelessTransport(pdn->getWirelessManager());
    quickdrawWirelessManager->initialize(player, pdn->getWirelessManager(), gWirelessTransport, 1000);
    symbolWirelessManager->initialize(pdn->getWirelessManager(), pdn->getRemoteDeviceCoordinator());

    // Register ESP-NOW packet handlers
    setupEspNow(quickdrawWirelessManager, remoteDebugManager, symbolWirelessManager, peerCommsDriver);
    
    game = new Quickdraw(player, pdn, quickdrawWirelessManager, remoteDebugManager, symbolWirelessManager, gWirelessTransport);
    
    pdn->getDisplay()->
    invalidateScreen()->
        drawImage(getImageForAllegiance(Allegiance::ALLEYCAT, ImageType::LOGO_LEFT))->
        drawImage(getImageForAllegiance(Allegiance::ALLEYCAT, ImageType::STAMP))->
        render();
    delay(3000);

    // Register state machines with the device and launch Quickdraw
    AppConfig apps = {
        {StateId(QUICKDRAW_APP_ID), game}
    };
    pdn->loadAppConfig(apps, StateId(QUICKDRAW_APP_ID));

    // Hand connectivity timing to a dedicated task so it stops sharing the main
    // loop's jitter. setExternalConnectivityTask(true) makes rdc->sync() skip
    // its inline serviceConnectivity call (native tests keep driving it inline).
    // Priority 2 sits just above the Arduino loop (1) so the 20ms cadence holds
    // under load; pinned to APP_CPU to leave PRO_CPU for the WiFi/ESP-NOW stack.
    if (auto* rdc = pdn->getRemoteDeviceCoordinator()) {
        rdc->setExternalConnectivityTask(true);
        xTaskCreatePinnedToCore(connectivityTask, "pdn-conn", 4096, rdc, 2,
                                nullptr, APP_CPU_NUM);
    }
}

void loop() {
    pdn->loop();
    if (gWirelessTransport) gWirelessTransport->sync();
}
