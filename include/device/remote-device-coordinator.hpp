#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include "device/serial-manager.hpp"
#include "device/mac-types.hpp"
#include "device/peer-graph.hpp"
#include "utils/simple-timer.hpp"
#include "wireless/handshake-wireless-manager.hpp"
#include "device/device-type.hpp"

class Device;
class HandshakeApp;
struct Frame;

enum class PortStatus {
    DISCONNECTED, // No direct peer on this jack (macPeer slot empty).
    CONNECTED,    // A direct peer is present on this jack.
};

class RemoteDeviceCoordinator {
public:
    // Nested to avoid collision with ::Peer (handshake-wireless-manager.hpp),
    // which carries additional fields (macAddr/sid/deviceType) for its own
    // ownership model.
    struct Peer {
        std::array<uint8_t, 6> mac;
        DeviceType deviceType;
    };

    // previous = std::nullopt on connect; current = std::nullopt on disconnect.
    // Both populated never happens — RDC fires only on presence transitions.
    using DirectPeerChangeCallback =
        std::function<void(SerialIdentifier port,
                           std::optional<Peer> previous,
                           std::optional<Peer> current)>;

    RemoteDeviceCoordinator();
    ~RemoteDeviceCoordinator();

    /**
     * Initialize should create a HandshakeWirelessManager, as well as
     * the handshake state machines. It should also
     * register the handshakeWirelessManager's packet received callback.
     */
    void initialize(WirelessManager* wirelessManager,
                    SerialManager* serialManager,
                    Device* PDN);

    /**
     * Must be called every loop tick (from PDN::loop).
     * Drives both handshake state machines.
     */
    void sync(Device* PDN);

    virtual PortStatus getPortStatus(SerialIdentifier port);

    // Returns a pointer to the peer's MAC address for the given port, or nullptr
    // if no peer is connected.
    virtual const uint8_t* getPeerMac(SerialIdentifier port) const;

    virtual DeviceType getPeerDeviceType(SerialIdentifier port) const;

    // Returns true iff `mac` matches the direct peer on either jack.
    virtual bool isDirectPeer(const uint8_t* mac) const;

    // Fires on direct-peer presence transitions on each jack:
    //   connect:    previous = nullopt, current = Peer{newMac, type}
    //   disconnect: previous = Peer{oldMac, UNKNOWN}, current = nullopt
    // The disconnect Peer carries the just-dropped MAC so subscribers can take
    // MAC-specific action (Shootout PEER_LOST, etc.). DeviceType is UNKNOWN
    // because RDC doesn't preserve it across the drop.
    void setOnDirectPeerChange(DirectPeerChangeCallback cb);

    void registerPeer(const uint8_t* macAddress);
    void unregisterPeer(const uint8_t* macAddress);

    // Connected component of self under the peer-graph's mutual edges. Order is
    // implementation-defined (callers must treat as a set). Virtual so test
    // fixtures can subclass RDC and inject deterministic snapshots.
    virtual std::vector<net::Mac> getChainMembers() const { return peerGraph_.getChainMembers(); }

    // True iff self is in a cycle of mutual edges, i.e. the ring is closed.
    // Virtual for test override.
    virtual bool isInLoop() const { return peerGraph_.isInLoop(); }

    // Run serviceConnectivity (HELLO emit + silent-link watchdog) on a fixed
    // cadence — call this from a hardware timer/task. While set, sync() stops
    // running serviceConnectivity itself (the task owns it). Native leaves it
    // off, so sync() drives everything single-threaded for the tests.
    void serviceConnectivity(unsigned long now);
    void setExternalConnectivityTask(bool on) { externalConnectivityTask_ = on; }

    // Count of chain members reachable through the direct peer on `jack`,
    // excluding self — i.e. how many devices are chained behind us on that
    // side. Returns 0 if no direct peer is present on the jack. Virtual so
    // test fixtures can inject deterministic chain depths.
    virtual size_t countChainBehind(SerialIdentifier jack) const {
        const uint8_t* peer = getPeerMac(jack);
        if (peer == nullptr) return 0;
        net::Mac firstHop;
        std::copy_n(peer, 6, firstHop.begin());
        return peerGraph_.countReachableExcludingSelf(firstHop);
    }

    // Drain recvQueue_, applying graph/macPeer mutations and BEACON floods
    // single-threaded — the connectivity analog of EspNowManager::exec()
    // (serial driver exec drains the UART, peer-comms exec drains its
    // recvQueue_, this exec drains the connectivity recvQueue_). Called at the
    // top of sync() in production; test fixtures call it to apply a delivered
    // frame without a full sync().
    void exec();

    // True iff no mutual edge has been added or removed in the last 200ms.
    // Consumers gate state derivation on this so brief topology churn (cable
    // wiggle, single dropped HELLO) doesn't thrash the game layer.
    virtual bool isTopologyStable() const { return peerGraph_.isTopologyStable(nowMs()); }

    // Fires synchronously whenever a mutual edge is added or removed.
    void setOnGraphChanged(std::function<void()> cb) {
        peerGraph_.setOnGraphChanged(std::move(cb));
    }

    // Toggle GPIO-level cable-disconnect detection: a yanked cable lets the RX
    // line float HIGH against the internal pullup and hits streak=10 within
    // ~1ms, while a connected jack stays at streak=0 (remote-driven LOW resets
    // the counter on each data bit). Off by default — see
    // enableGpioDisconnectDetection_ for why. Tests use this to drive the
    // synthetic disconnect path in the multi-device fixture.
    void setGpioDisconnectDetectionEnabled(bool on) {
        enableGpioDisconnectDetection_ = on;
    }

    // Test hook to override the silent-link jack-dead threshold (production:
    // kHelloSilentLinkMs). A single-device unit test has no partner emitting
    // HELLOs, so the production threshold trips during any multi-second time
    // advance (e.g. primeRosterStable's 3.3s prime window). Tests that prime
    // then exercise long-running behavior call this with a longer threshold.
    void setJackDeadSilentLinkMsForTest(unsigned long ms) {
        helloSilentLinkMs_ = ms;
    }

    // Per-jack diagnostic counters, read by tests via getRosterStats. uint32_t
    // (not uint16_t) because at the HELLO/BEACON frame rate a 16-bit counter
    // wraps in minutes, shorter than a LARP round; 32-bit is effectively
    // unbounded.
    struct RosterStats {
        uint32_t framesRx = 0;
        uint32_t framesTx = 0;
        uint32_t framesCrcFail = 0;
        uint32_t parserResyncs = 0;
        uint32_t jacksDeclaredDead = 0;
    };

    const RosterStats& getRosterStats(SerialIdentifier jack) const {
        return stats_[jack == SerialIdentifier::INPUT_JACK ? 0 : 1];
    }

private:
    std::array<RosterStats, 2> stats_;
    std::array<std::optional<std::array<uint8_t, 6>>, 2> previousDirectPeer_;

    // Silent-link watchdog threshold = disconnect-detection latency. A CONNECTED
    // jack with no HELLO in this window is declared dead. Peers emit HELLO every
    // 20ms (connectivityTask in main.cpp) and RX is timestamped on the UART event
    // task within ~1ms of arrival, so the worst-case inter-HELLO gap is ~one emit
    // period plus scheduling slack. 100ms is 5 missed emits: fast enough to feel
    // instant, with margin against transient task slack. Overridable via
    // setJackDeadSilentLinkMsForTest.
    static constexpr unsigned long kHelloSilentLinkMs = 100;
    unsigned long helloSilentLinkMs_ = kHelloSilentLinkMs;

    // GPIO disconnect detection is OFF by default: on the real PDN PCB the
    // OUTPUT-jack RX (GPIO 38 = TXr) is wired to the cable TIP conductor, the
    // least-reliable TRS contact, so on a marginal/flexing connection the line +
    // the driver's pullup float HIGH and the line-state read false-fires,
    // spuriously clearing macPeer. (The onboard-RGB-LED-on-38 story is a
    // dev-board-only fact, not the real-board cause.) The HELLO silent-link
    // (kHelloSilentLinkMs) is a reliable disconnect path that makes the GPIO read
    // redundant. Tests opt specific jacks in via setGpioDisconnectDetectionEnabled.
    bool enableGpioDisconnectDetection_ = false;

    // Surrender a jack: clear its direct-peer slot, fire the synthetic
    // disconnect callback, reset the handshake. reconcileSelfPeers() on the
    // next sync() propagates the now-cleared edge via an updated BEACON.
    void declareJackDead(SerialIdentifier jack, Device* PDN);

    unsigned long nowMs() const;

    // Single writeBytes site for binary frames: increments framesTx.
    void emitFrame(SerialIdentifier jack, const std::vector<uint8_t>& frame);

    uint8_t portIndex(SerialIdentifier port) const;

    static SerialIdentifier oppositeJack(SerialIdentifier j) {
        return j == SerialIdentifier::INPUT_JACK
            ? SerialIdentifier::OUTPUT_JACK : SerialIdentifier::INPUT_JACK;
    }


    SerialManager* serialManager = nullptr;
    WirelessManager* wirelessManager_ = nullptr;
    DirectPeerChangeCallback onDirectPeerChange_;

    void fireDirectPeerChange(SerialIdentifier port,
                              std::optional<Peer> previous,
                              std::optional<Peer> current);

    HandshakeWirelessManager handshakeWirelessManager;

    HandshakeApp* inputPortHandshake = nullptr;
    HandshakeApp* outputPortHandshake = nullptr;

    PeerGraph peerGraph_;

    // A received serial frame, parsed but not yet applied. Deliberately named to
    // match EspNowManager: the receive path (ingestSerial) only validates +
    // enqueues a DeferredPacket onto recvQueue_; the main loop drains it via
    // exec() and runs the handlers single-threaded — identical to how the
    // ESP-NOW recv callback enqueues and exec() drains. So the peer-graph needs
    // no lock.
    struct DeferredPacket {
        enum Kind { HELLO, BEACON, JACK_SILENT } kind;
        SerialIdentifier jack;
        net::Mac mac{};               // HELLO source
        DeviceType deviceType = DeviceType::UNKNOWN;
        BeaconRecord beacon{};                 // BEACON payload
    };
    std::queue<DeferredPacket> recvQueue_;
    // Guards recvQueue_ across the receive callback / connectivity task /
    // main-loop drain boundary. Held only to push or to swap the batch out
    // (exec processes the batch lock-free) — same idiom as EspNowManager.
    std::mutex recvMutex_;

    // Validate + enqueue a received frame (no graph/macPeer mutation here).
    void ingestSerial(SerialIdentifier jack, const Frame& frame);

    // serviceConnectivity (HELLO emit + watchdog) is declared in the public
    // section so a hardware task can drive it. State it needs:
    unsigned long lastHelloEmitMs_ = 0;
    static constexpr unsigned long kHelloEmitMs = 20;
    bool externalConnectivityTask_ = false;        // set when a task owns it
    DeviceType selfDeviceType_ = DeviceType::PDN;  // cached at initialize()
    Device* pdn_ = nullptr;                        // cached at sync() entry

    // Per-jack timestamp of the last HELLO received — the liveness truth. Atomic
    // because the receive callback writes it while the watchdog (which may run on
    // a separate task) reads it. The silent-link watchdog declares a jack dead
    // when now - this exceeds kHelloSilentLinkMs. Initialised in the constructor.
    std::array<std::atomic<unsigned long>, 2> lastHelloRxMs_;

    // Last (in, out) self-peer MACs reflected into peerGraph_ and broadcast as
    // a BEACON. sync() reconciles the current macPeer slots against this; any
    // difference triggers a PeerGraph update + fresh BEACON emit on both jacks.
    net::Mac lastEmittedInPeer_{};
    net::Mac lastEmittedOutPeer_{};

    // Reconcile current macPeer slots into peerGraph_ and broadcast a BEACON
    // when they have changed since the last emit. Called once per sync().
    void reconcileSelfPeers();

    // Periodic BEACON re-emit cadence (convergence backstop, deduped by
    // receivers so it only propagates when it carries new information).
    unsigned long lastBeaconBackstopMs_ = 0;
    static constexpr unsigned long kBeaconBackstopMs = 1000;

    // Emit this device's current BEACON on both jacks.
    void emitBeaconBothJacks();
};
