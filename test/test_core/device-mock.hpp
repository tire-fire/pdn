//
// Created by Elli Furedy on 10/6/2024.
//
#pragma once

#include <gmock/gmock.h>
#include "device/device.hpp"
#include "device/remote-device-coordinator.hpp"
#include "device/drivers/display.hpp"
#include "device/drivers/button.hpp"
#include "device/drivers/haptics.hpp"
#include "device/drivers/http-client-interface.hpp"
#include "device/drivers/peer-comms-interface.hpp"
#include "device/drivers/storage-interface.hpp"
#include "device/light-manager.hpp"
#include "wireless/quickdraw-wireless-manager.hpp"
#include "game/chain-manager.hpp"
#include <queue>
#include <vector>

using namespace std;

class FakeHWSerialWrapper : public HWSerialWrapper {
    public:
    FakeHWSerialWrapper() : HWSerialWrapper() {}

    int availableForWrite() override {
        return 1024 - msgQueue.size();
    }

    int available() override {
        return msgQueue.size() > 0;
    }

    void flush() override {
        msgQueue.clear();
    }

    void setBytesCallback(const SerialBytesCallback& callback) override {
        bytesCallback = callback;
    }

    void writeBytes(const uint8_t* data, size_t len) override {
        for (size_t i = 0; i < len; ++i) {
            msgQueue.emplace_back(static_cast<char>(data[i]));
        }
    }

    deque<char> msgQueue;
    SerialBytesCallback bytesCallback;
};

class FakeDevice : public Device {
};

// Mock classes for each interface
class MockDisplay : public Display {
public:
    MOCK_METHOD(Display*, invalidateScreen, (), (override));
    MOCK_METHOD(void, render, (), (override));
    MOCK_METHOD(Display*, drawText, (const char*), (override));
    MOCK_METHOD(Display*, setGlyphMode, (FontMode), (override));
    MOCK_METHOD(Display*, renderGlyph, (const char*, int, int), (override));
    MOCK_METHOD(Display*, drawButton, (const char*, int, int), (override));
    MOCK_METHOD(Display*, drawText, (const char*, int, int), (override));
    MOCK_METHOD(Display*, drawImage, (Image), (override));
    MOCK_METHOD(Display*, drawImage, (Image, int, int), (override));
    MOCK_METHOD(int, getTextWidth, (const char*), (override));
    MOCK_METHOD(int, getWidth, (), (override));
    MOCK_METHOD(Display*, whiteScreen, (), (override));
    MOCK_METHOD(Display*, whiteScreenLeftHalf, (), (override));
    MOCK_METHOD(Display*, whiteScreenRightHalf, (), (override));
};

class MockButton : public Button {
public:
    MOCK_METHOD(void, setButtonPress, (callbackFunction, ButtonInteraction), (override));
    MOCK_METHOD(void, setButtonPress, (parameterizedCallbackFunction, void*, ButtonInteraction), (override));
    MOCK_METHOD(void, removeButtonCallbacks, (), (override));
    MOCK_METHOD(bool, isLongPressed, (), (override));
    MOCK_METHOD(unsigned long, longPressedMillis, (), (override));
};

class MockHaptics : public Haptics {
public:
    MOCK_METHOD(bool, isOn, (), (override));
    MOCK_METHOD(void, max, (), (override));
    MOCK_METHOD(void, setIntensity, (int), (override));
    MOCK_METHOD(int, getIntensity, (), (override));
    MOCK_METHOD(void, off, (), (override));
};

class MockHttpClient : public HttpClientInterface {
public:
    MOCK_METHOD(void, setWifiConfig, (WifiConfig*), (override));
    MOCK_METHOD(bool, isConnected, (), (override));
    MOCK_METHOD(bool, queueRequest, (HttpRequest&), (override));
    MOCK_METHOD(void, disconnect, (), (override));
    MOCK_METHOD(void, updateConfig, (WifiConfig*), (override));
    MOCK_METHOD(void, retryConnection, (), (override));
    MOCK_METHOD(uint8_t*, getMacAddress, (), (override));
    MOCK_METHOD(void, setHttpClientState, (HttpClientState), (override));
    MOCK_METHOD(HttpClientState, getHttpClientState, (), (override));
};

class MockPeerComms : public PeerCommsInterface {
public:
    MOCK_METHOD(int, sendData, (const uint8_t*, PktType, const uint8_t*, const size_t), (override));
    MOCK_METHOD(void, setPacketHandler, (PktType, PacketCallback, void*), (override));
    MOCK_METHOD(void, clearPacketHandler, (PktType), (override));
    MOCK_METHOD(const uint8_t*, getGlobalBroadcastAddress, (), (override));
    MOCK_METHOD(uint8_t*, getMacAddress, (), (override));
    MOCK_METHOD(void, removePeer, (uint8_t*), (override));
    MOCK_METHOD(int, addEspNowPeer, (const uint8_t*), (override));
    MOCK_METHOD(int, removeEspNowPeer, (const uint8_t*), (override));
    MOCK_METHOD(void, connect, (), (override));
    MOCK_METHOD(void, disconnect, (), (override));
    MOCK_METHOD(void, setPeerCommsState, (PeerCommsState), (override));
    MOCK_METHOD(PeerCommsState, getPeerCommsState, (), (override));
};

class MockStorage : public StorageInterface {
public:
    MOCK_METHOD(size_t, write, (const std::string&, const std::string&), (override));
    MOCK_METHOD(std::string, read, (const std::string&, const std::string&), (override));
    MOCK_METHOD(bool, remove, (const std::string&), (override));
    MOCK_METHOD(bool, clear, (), (override));
    MOCK_METHOD(void, end, (), (override));
    MOCK_METHOD(uint8_t, readUChar, (const std::string&, uint8_t), (override));
    MOCK_METHOD(size_t, writeUChar, (const std::string&, uint8_t), (override));
};

class FakeRemoteDeviceCoordinator : public RemoteDeviceCoordinator {
public:
    void setPortStatus(SerialIdentifier id, PortStatus status) {
        if (id == SerialIdentifier::OUTPUT_JACK) outputStatus = status;
        else if (id == SerialIdentifier::INPUT_JACK) inputStatus = status;
    }

    PortStatus getPortStatus(SerialIdentifier id) override {
        if (id == SerialIdentifier::OUTPUT_JACK) return outputStatus;
        if (id == SerialIdentifier::INPUT_JACK) return inputStatus;
        return PortStatus::DISCONNECTED;
    }

    void setPeerDeviceType(SerialIdentifier id, DeviceType type) {
        if (id == SerialIdentifier::OUTPUT_JACK) outputDeviceType = type;
        else if (id == SerialIdentifier::INPUT_JACK) inputDeviceType = type;
    }

    DeviceType getPeerDeviceType(SerialIdentifier id) const override {
        if (id == SerialIdentifier::OUTPUT_JACK) return outputDeviceType;
        if (id == SerialIdentifier::INPUT_JACK) return inputDeviceType;
        return DeviceType::UNKNOWN;
    }

    // Stubbable direct peer MACs — tests that exercise peer-MAC gating set
    // these; getPeerMac returns a pointer into the stored array or nullptr.
    void setPeerMac(SerialIdentifier id, const uint8_t* mac) {
        if (id == SerialIdentifier::OUTPUT_JACK) {
            outputPeerSet = (mac != nullptr);
            if (mac) memcpy(outputPeerMac, mac, 6);
        } else if (id == SerialIdentifier::INPUT_JACK) {
            inputPeerSet = (mac != nullptr);
            if (mac) memcpy(inputPeerMac, mac, 6);
        }
    }

    const uint8_t* getPeerMac(SerialIdentifier id) const override {
        if (id == SerialIdentifier::OUTPUT_JACK && outputPeerSet) return outputPeerMac;
        if (id == SerialIdentifier::INPUT_JACK && inputPeerSet) return inputPeerMac;
        return nullptr;
    }

private:
    PortStatus outputStatus = PortStatus::DISCONNECTED;
    PortStatus inputStatus = PortStatus::DISCONNECTED;
    DeviceType outputDeviceType = DeviceType::UNKNOWN;
    DeviceType inputDeviceType = DeviceType::UNKNOWN;
    uint8_t outputPeerMac[6] = {};
    uint8_t inputPeerMac[6] = {};
    bool outputPeerSet = false;
    bool inputPeerSet = false;
};

// Stand-in CDM for tests that flip isCoordinator() without standing up the
// full handshake stack. Used by ShootoutProposal/BracketReveal debounce
// tests to simulate a coordinator-loss event without driving the whole
// loop-close pipeline.
class FakeChainManager : public ChainManager {
public:
    FakeChainManager(Player* p, WirelessManager* wm, RemoteDeviceCoordinator* rdc)
        : ChainManager(p, wm, rdc) {}
    bool isCoordinator() const override { return isCoordinator_; }
    void setIsCoordinator(bool v) { isCoordinator_ = v; }
    // Stub the settled-topology signals so tests can drive loop presence/loss
    // directly instead of priming a real roster. Default to "in a stable loop"
    // since shootout tests run inside a ring.
    bool isInStableLoop() const override { return inStableLoop_; }
    void setInStableLoop(bool v) { inStableLoop_ = v; }
    bool isTopologyStable() const override { return rosterStable_; }
    void setRosterStable(bool v) { rosterStable_ = v; }
private:
    bool isCoordinator_ = true;
    bool inStableLoop_ = true;
    bool rosterStable_ = true;
};

// Fake QuickdrawWirelessManager that captures outbound packets instead of transmitting them.
// Call deliverLastTo() to route the most-recently-captured packet into another manager's
// transport (deliverIncoming), exercising the real serialization/deserialization path.
class FakeQuickdrawWirelessManager : public QuickdrawWirelessManager {
public:
    FakeQuickdrawWirelessManager() : QuickdrawWirelessManager() {}

    int broadcastPacket(const uint8_t* /*macAddress*/, QuickdrawCommand& command) override {
        sentCommands.push_back(command);
        return 0;
    }

    int broadcastReliable(const uint8_t* /*macAddress*/, QuickdrawCommand& command) override {
        // Funnels into sentCommands like broadcastPacket so test assertions are uniform.
        // This skips the channel, so command.seqId stays 0 and deliverLastTo injects
        // a fire-and-forget packet (no ack/dedup). Channel-owned reliable dedup is
        // covered directly by PacketParsingTests.dedupsResentReliablePacket.
        sentCommands.push_back(command);
        return 0;
    }

    void deliverLastTo(QuickdrawWirelessManager* recipient, const uint8_t* senderMac) {
        if (sentCommands.empty()) return;
        const QuickdrawCommand& cmd = sentCommands.back();

        // Serialize into the same wire format used by the real broadcastPacket.
        QuickdrawPacket pkt = {};
        memcpy(pkt.matchId,  cmd.matchId,  sizeof(pkt.matchId));
        memcpy(pkt.playerId, cmd.playerId, sizeof(pkt.playerId));
        pkt.isHunter       = cmd.isHunter;
        pkt.playerDrawTime = cmd.playerDrawTime;
        pkt.command        = cmd.command;
        pkt.seqId          = cmd.seqId;

        // Inject at the same boundary the radio uses: the recipient's transport
        // acks, dedups, and dispatches via the channel's onReceive.
        recipient->getTransport()->deliverIncoming(
            PktType::kQuickdrawCommand, 0, senderMac,
            reinterpret_cast<const uint8_t*>(&pkt),
            sizeof(pkt));
    }

    std::vector<QuickdrawCommand> sentCommands;
};

// Fake light strip for LightManager
class FakeLightStrip : public LightStrip {
public:
    void setLight(LightIdentifier lightSet, uint8_t index, LEDState::SingleLEDState color) override {}
    void setLightBrightness(LightIdentifier lightSet, uint8_t index, uint8_t brightness) override {}
    void setGlobalBrightness(uint8_t brightness) override {}
    LEDState::SingleLEDState getLight(LightIdentifier lightSet, uint8_t index) override { 
        return LEDState::SingleLEDState(); 
    }
    void fade(LightIdentifier lightSet, uint8_t fadeAmount) override {}
    void addToLight(LightIdentifier lightSet, uint8_t index, LEDState::SingleLEDState color) override {}
    void setFPS(uint8_t fps) override {}
    uint8_t getFPS() const override { return 0; }
};

class MockDevice : public Device {
public:
    MockDevice() : Device(DriverConfig{}) {
        // Initialize mock pointers
        mockDisplay = new MockDisplay();
        mockPrimaryButton = new MockButton();
        mockSecondaryButton = new MockButton();
        mockHaptics = new MockHaptics();
        mockHttpClient = new MockHttpClient();
        // NiceMock: the multi-device fixture calls getMacAddress/getPeerCommsState
        // tens of thousands of times with only ON_CALL defaults (no EXPECT_CALL).
        // Plain mocks emit a ~5-line "uninteresting call" warning per call —
        // ~333K lines of output that `pio test`'s per-line parser then spends
        // minutes chewing through. NiceMock suppresses the warnings; ON_CALL
        // defaults and any EXPECT_CALL expectations still apply.
        mockPeerComms = new testing::NiceMock<MockPeerComms>();
        mockStorage = new MockStorage();
        lightManager = new LightManager(fakeLightStrip);
        serialManager = new SerialManager(&outputJackSerial, &inputJackSerial);
        wirelessManager = new WirelessManager(mockPeerComms, mockHttpClient);
    }

    ~MockDevice() {
        delete mockDisplay;
        delete mockPrimaryButton;
        delete mockSecondaryButton;
        delete mockHaptics;
        delete mockHttpClient;
        delete mockPeerComms;
        delete mockStorage;
        delete lightManager;
        delete serialManager;
        delete wirelessManager;
    }

    // Device Methods
    MOCK_METHOD(int, begin, (), (override));
    MOCK_METHOD(void, setDeviceId, (const std::string&), (override));
    MOCK_METHOD(std::string, getDeviceId, (), (override));
    DeviceType getDeviceType() override { return DeviceType::PDN; }

    // Getters return mock instances
    Display* getDisplay() override { return mockDisplay; }
    Button* getPrimaryButton() override { return mockPrimaryButton; }
    Button* getSecondaryButton() override { return mockSecondaryButton; }
    Haptics* getHaptics() override { return mockHaptics; }
    HttpClientInterface* getHttpClient() override { return mockHttpClient; }
    PeerCommsInterface* getPeerComms() override { return mockPeerComms; }
    StorageInterface* getStorage() override { return mockStorage; }
    LightManager* getLightManager() override { return lightManager; }
    WirelessManager* getWirelessManager() override { return wirelessManager; }
    SerialManager* getSerialManager() override { return serialManager; }
    RemoteDeviceCoordinator* getRemoteDeviceCoordinator() override {
        return rdcOverride ? rdcOverride : &fakeRemoteDeviceCoordinator;
    }
    void setRdcOverride(RemoteDeviceCoordinator* rdc) { rdcOverride = rdc; }

    // Mock interface instances
    MockDisplay* mockDisplay;
    MockButton* mockPrimaryButton;
    MockButton* mockSecondaryButton;
    MockHaptics* mockHaptics;
    MockHttpClient* mockHttpClient;
    MockPeerComms* mockPeerComms;
    MockStorage* mockStorage;
    FakeLightStrip fakeLightStrip;
    LightManager* lightManager;
    SerialManager* serialManager;
    WirelessManager* wirelessManager;

    FakeHWSerialWrapper outputJackSerial;
    FakeHWSerialWrapper inputJackSerial;
    FakeRemoteDeviceCoordinator fakeRemoteDeviceCoordinator;
    RemoteDeviceCoordinator* rdcOverride = nullptr;
};
