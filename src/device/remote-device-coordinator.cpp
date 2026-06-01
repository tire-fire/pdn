#include "device/remote-device-coordinator.hpp"
#include "apps/handshake/handshake.hpp"
#include "device/peer-graph-codec.hpp"
#include "device/device-constants.hpp"
#include "device/drivers/serial-wrapper.hpp"
#include "device/serial-manager.hpp"
#include "device/drivers/logger.hpp"
#include "device/wireless-manager.hpp"
#include "state/state-machine.hpp"
#include "utils/simple-timer.hpp"
#include "wireless/handshake-wireless-manager.hpp"
#include "wireless/mac-functions.hpp"
#include "wireless/peer-comms-types.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>

#define TAG "RDC"

RemoteDeviceCoordinator::RemoteDeviceCoordinator() : handshakeWirelessManager(HandshakeWirelessManager()) {
    lastHelloRxMs_[0] = 0;
    lastHelloRxMs_[1] = 0;
}

RemoteDeviceCoordinator::~RemoteDeviceCoordinator() {
    delete inputPortHandshake;
    delete outputPortHandshake;
}


void RemoteDeviceCoordinator::initialize(WirelessManager* wirelessManager, SerialManager* serialManager, Device* PDN) {
    this->serialManager = serialManager;
    this->wirelessManager_ = wirelessManager;
    if (PDN != nullptr) selfDeviceType_ = PDN->getDeviceType();

    handshakeWirelessManager.initialize(wirelessManager);

    if (wirelessManager_ != nullptr) {
        const uint8_t* mac = wirelessManager_->getMacAddress();
        if (mac != nullptr) {
            net::Mac selfMac;
            std::copy_n(mac, 6, selfMac.begin());
            peerGraph_.setSelfMac(selfMac);
        }
    }

    inputPortHandshake = new HandshakeApp(SerialIdentifier::INPUT_JACK);
    outputPortHandshake = new HandshakeApp(SerialIdentifier::OUTPUT_JACK);

    // start() wires the serial wrapper's bytes callback to HandshakeApp's binary
    // demuxer; without it the byte path is unhooked.
    inputPortHandshake->start(serialManager);
    outputPortHandshake->start(serialManager);

    inputPortHandshake->setBinaryFrameHandler([this](const Frame& f) {
        ingestSerial(SerialIdentifier::INPUT_JACK, f);
    });
    outputPortHandshake->setBinaryFrameHandler([this](const Frame& f) {
        ingestSerial(SerialIdentifier::OUTPUT_JACK, f);
    });

    // Plumb the byte demuxer's CRC-fail and parser-resync events into the
    // per-jack RosterStats. The parser lives inside HandshakeApp; without these
    // callbacks RDC cannot observe sub-protocol byte-level corruption events.
    inputPortHandshake->setCrcFailHandler([this]() {
        stats_[portIndex(SerialIdentifier::INPUT_JACK)].framesCrcFail++;
    });
    outputPortHandshake->setCrcFailHandler([this]() {
        stats_[portIndex(SerialIdentifier::OUTPUT_JACK)].framesCrcFail++;
    });
    inputPortHandshake->setParserResyncHandler([this]() {
        stats_[portIndex(SerialIdentifier::INPUT_JACK)].parserResyncs++;
    });
    outputPortHandshake->setParserResyncHandler([this]() {
        stats_[portIndex(SerialIdentifier::OUTPUT_JACK)].parserResyncs++;
    });
}

void RemoteDeviceCoordinator::ingestSerial(SerialIdentifier jack,
                                          const Frame& frame) {
    using namespace net;
    // Receive path: validate, drop self, stamp liveness, enqueue. No graph or
    // macPeer mutation here — exec() does that single-threaded on the
    // main loop (see DeferredPacket). The parser delivers frames only after CRC.
    stats_[portIndex(jack)].framesRx++;
    const unsigned long activityNow = nowMs();

    if (frame.opcode == peer_graph::kOpHello) {
        // HELLO: source is our direct peer on this jack; also the silent-link
        // liveness signal. Payload = source mac[6] + deviceType[1].
        if (frame.payload.size() != 7) return;
        Mac peerMac;
        std::copy_n(frame.payload.begin(), 6, peerMac.begin());
        if (!isValidPeerMac(peerMac)) return;
        // Reject self-sourced HELLOs BEFORE the liveness stamp. The OUTPUT jack
        // can echo our own HELLO; counting that echo as liveness keeps the
        // silent-link from ever firing on a yanked OUTPUT cable.
        if (peerMac == peerGraph_.getSelfMac()) return;
        lastHelloRxMs_[portIndex(jack)] = activityNow;
        DeferredPacket ev;
        ev.kind = DeferredPacket::HELLO;
        ev.jack = jack;
        ev.mac = peerMac;
        ev.deviceType = static_cast<DeviceType>(frame.payload[6]);
        { std::lock_guard<std::mutex> lk(recvMutex_); recvQueue_.push(ev); }
        return;
    }

    if (frame.opcode == peer_graph::kOpBeacon) {
        // BEACON: a flooded topology frame. Payload = source+inPeer+outPeer.
        if (frame.payload.size() != 18) return;
        BeaconRecord beacon;
        std::copy_n(frame.payload.begin(), 6, beacon.source.begin());
        std::copy_n(frame.payload.begin() + 6, 6, beacon.inPeer.begin());
        std::copy_n(frame.payload.begin() + 12, 6, beacon.outPeer.begin());
        // Our own beacon came back around the ring: drop without forwarding.
        if (beacon.source == peerGraph_.getSelfMac()) return;
        DeferredPacket ev;
        ev.kind = DeferredPacket::BEACON;
        ev.jack = jack;
        ev.beacon = beacon;
        { std::lock_guard<std::mutex> lk(recvMutex_); recvQueue_.push(ev); }
        return;
    }
}

void RemoteDeviceCoordinator::exec() {
    using namespace net;
    // Swap the batch out under the lock, then process it lock-free — the
    // handlers (setMacPeer/ESP-NOW registration, BEACON flood, declareJackDead)
    // are slow and must not run while the receive callback waits to enqueue.
    // Identical to EspNowManager::exec().
    std::queue<DeferredPacket> batch;
    {
        std::lock_guard<std::mutex> lk(recvMutex_);
        std::swap(batch, recvQueue_);
    }
    const unsigned long now = nowMs();
    while (!batch.empty()) {
        DeferredPacket ev = batch.front();
        batch.pop();
        if (ev.kind == DeferredPacket::HELLO) {
            const ::Peer* cur = handshakeWirelessManager.getMacPeer(ev.jack);
            if (cur == nullptr || cur->macAddr != ev.mac) {
                LOG_D(TAG, "HELLO jack=%d from=%02X%02X%02X (was %s)",
                      (int)portIndex(ev.jack), ev.mac[3], ev.mac[4], ev.mac[5],
                      cur ? "set" : "empty");
            }
            ::Peer peer;
            peer.macAddr = ev.mac;
            peer.sid = ev.jack;
            peer.deviceType = ev.deviceType;
            // setMacPeer handles ESP-NOW registration; reconcileSelfPeers()
            // later in sync() reflects the change into the peer-graph + BEACON.
            handshakeWirelessManager.setMacPeer(ev.jack, peer);
        } else if (ev.kind == DeferredPacket::BEACON) {
            // Cache; if the content changed, flood onward on the opposite jack.
            if (peerGraph_.acceptBeacon(ev.beacon, now)) {
                auto forward = peer_graph::encodeBeacon(ev.beacon);
                emitFrame(oppositeJack(ev.jack), forward);
            }
        } else if (ev.kind == DeferredPacket::JACK_SILENT) {
            // The watchdog (serviceConnectivity) flagged this jack dead. Do the
            // macPeer/handshake teardown here so it stays single-threaded.
            declareJackDead(ev.jack, pdn_);
        }
    }
}

void RemoteDeviceCoordinator::emitFrame(SerialIdentifier jack,
                                       const std::vector<uint8_t>& frame) {
    if (!serialManager) return;
    serialManager->writeBytes(frame.data(), frame.size(), jack);
    stats_[portIndex(jack)].framesTx++;
}

unsigned long RemoteDeviceCoordinator::nowMs() const {
    auto* clk = SimpleTimer::getPlatformClock();
    return clk ? clk->milliseconds() : 0;
}

void RemoteDeviceCoordinator::serviceConnectivity(unsigned long now) {
    // (1) Emit HELLO on both jacks at a fixed cadence — the liveness signal our
    // neighbors watch. Always-on (gating on cable status deadlocks bootstrap).
    // Writes directly via the serial manager, NOT emitFrame, so the steady HELLO
    // stream doesn't inflate the framesTx diagnostic counter.
    if (serialManager != nullptr && now - lastHelloEmitMs_ >= kHelloEmitMs) {
        lastHelloEmitMs_ = now;
        auto frame = peer_graph::encodeHello(peerGraph_.getSelfMac(),
                                             static_cast<uint8_t>(selfDeviceType_));
        serialManager->writeBytes(frame.data(), frame.size(), SerialIdentifier::INPUT_JACK);
        serialManager->writeBytes(frame.data(), frame.size(), SerialIdentifier::OUTPUT_JACK);
    }

    // (2) Silent-link watchdog. Gates on the atomic lastHelloRxMs only — no
    // handshake-state read — so it is race-free when run from a dedicated task:
    // a jack whose HELLO baseline has gone stale is dead. baseline is 0 until
    // the first HELLO and reset to 0 on death, so a never-connected jack never
    // fires. Enqueue JACK_SILENT; exec() does the declareJackDead so all
    // macPeer/handshake teardown stays single-threaded.
    for (SerialIdentifier port : {SerialIdentifier::INPUT_JACK, SerialIdentifier::OUTPUT_JACK}) {
        const uint8_t idx = portIndex(port);
        const unsigned long baseline = lastHelloRxMs_[idx];
        // baseline can briefly exceed `now`: the RX path stamps lastHelloRxMs_
        // from another task and may land between the `now` capture at the top of
        // serviceConnectivity and this read. A HELLO that recent means the link
        // is alive, so clamp to gap 0 instead of letting unsigned subtraction
        // underflow to ~UINT32_MAX and fire a false silent-death.
        const unsigned long gap = baseline >= now ? 0 : now - baseline;
        const bool silentLinkExpired =
            baseline != 0 && gap > helloSilentLinkMs_;
        bool gpioDisconnected = false;
        if (auto* sm = pdn_ ? pdn_->getSerialManager() : nullptr) {
            HWSerialWrapper* serial =
                (port == SerialIdentifier::INPUT_JACK) ? sm->getInputJack() : sm->getOutputJack();
            if (serial != nullptr && enableGpioDisconnectDetection_) {
                gpioDisconnected = serial->isCableDisconnected();
            }
        }
        if (gpioDisconnected || silentLinkExpired) {
            LOG_D(TAG, "JACKDEAD jack=%d reason=%s gap=%lu", (int)idx,
                  gpioDisconnected ? "gpio" : "silent", gap);
            DeferredPacket ev;
            ev.kind = DeferredPacket::JACK_SILENT;
            ev.jack = port;
            { std::lock_guard<std::mutex> lk(recvMutex_); recvQueue_.push(ev); }
        }
    }
    // The 1Hz BEACON backstop stays on the main loop (sync), not here: it reads
    // lastEmitted{In,Out}Peer_, which reconcileSelfPeers writes on the main loop.
}

void RemoteDeviceCoordinator::sync(Device* PDN) {
    pdn_ = PDN;
    const unsigned long now = nowMs();

    // Connectivity producer: emit HELLO + run the watchdog. On hardware a 20ms
    // task owns this (externalConnectivityTask_ set), so sync() skips it; on
    // native it runs inline so the tests drive identical logic via sync().
    if (!externalConnectivityTask_) serviceConnectivity(now);

    // Drain the receive queue: RX HELLO/BEACON (macPeer, accept+flood) and the
    // watchdog's JACK_SILENT (declareJackDead) — all single-threaded — before
    // the direct-peer scan and reconcile read macPeer.
    exec();

    // Direct-peer presence edges → onDirectPeerChange (the game layer subscribes
    // for connect/disconnect). ESP-NOW peer registration on connect.
    for (SerialIdentifier port : {SerialIdentifier::INPUT_JACK, SerialIdentifier::OUTPUT_JACK}) {
        const ::Peer* directPeer = handshakeWirelessManager.getMacPeer(port);
        auto& prev = previousDirectPeer_[portIndex(port)];
        bool nowPresent = (directPeer != nullptr);
        bool wasPresent = prev.has_value();

        if (!nowPresent && wasPresent) {
            fireDirectPeerChange(port, Peer{*prev, DeviceType::UNKNOWN}, std::nullopt);
        }
        if (nowPresent && !wasPresent) {
            registerPeer(directPeer->macAddr.data());
            Peer current{directPeer->macAddr, directPeer->deviceType};
            fireDirectPeerChange(port, std::nullopt, current);
            // Seed the silent-link watchdog so it doesn't fire before the first
            // HELLO of the new connection has had a chance to arrive.
            lastHelloRxMs_[portIndex(port)] = now;
        }
        prev = nowPresent ? std::optional<std::array<uint8_t, 6>>{directPeer->macAddr}
                          : std::nullopt;
    }

    // Reconcile current macPeer slots into the peer-graph and broadcast a fresh
    // BEACON when they changed. Runs last so connect/disconnect/jack-dead
    // mutations from this tick are captured.
    reconcileSelfPeers();

    // Periodic BEACON backstop (1Hz). Main-loop only: it reads the self-peers
    // reconcileSelfPeers just wrote. Closes the cold-boot convergence gap; the
    // content-dedup at receivers stops it from flooding when unchanged.
    if (now - lastBeaconBackstopMs_ >= kBeaconBackstopMs) {
        lastBeaconBackstopMs_ = now;
        if (lastEmittedInPeer_ != net::Mac{} ||
            lastEmittedOutPeer_ != net::Mac{}) {
            emitBeaconBothJacks();
        }
    }
}

void RemoteDeviceCoordinator::reconcileSelfPeers() {
    net::Mac inPeer{};
    net::Mac outPeer{};
    if (const ::Peer* p = handshakeWirelessManager.getMacPeer(SerialIdentifier::INPUT_JACK)) {
        inPeer = p->macAddr;
    }
    if (const ::Peer* p = handshakeWirelessManager.getMacPeer(SerialIdentifier::OUTPUT_JACK)) {
        outPeer = p->macAddr;
    }
    if (inPeer == lastEmittedInPeer_ && outPeer == lastEmittedOutPeer_) return;
    LOG_D(TAG, "SELFPEERS in=%02X%02X%02X out=%02X%02X%02X",
          inPeer[3], inPeer[4], inPeer[5], outPeer[3], outPeer[4], outPeer[5]);
    lastEmittedInPeer_ = inPeer;
    lastEmittedOutPeer_ = outPeer;
    peerGraph_.setSelfPeers(inPeer, outPeer, nowMs());
    emitBeaconBothJacks();
}

void RemoteDeviceCoordinator::emitBeaconBothJacks() {
    BeaconRecord beacon;
    beacon.source = peerGraph_.getSelfMac();
    beacon.inPeer = lastEmittedInPeer_;
    beacon.outPeer = lastEmittedOutPeer_;
    auto frame = peer_graph::encodeBeacon(beacon);
    emitFrame(SerialIdentifier::INPUT_JACK, frame);
    emitFrame(SerialIdentifier::OUTPUT_JACK, frame);
}

uint8_t RemoteDeviceCoordinator::portIndex(SerialIdentifier port) const {
    return port == SerialIdentifier::INPUT_JACK ? 0 : 1;
}

PortStatus RemoteDeviceCoordinator::getPortStatus(SerialIdentifier port) {
    // Connectivity is direct-peer presence: macPeer is set in exec() when a
    // HELLO arrives on the jack and cleared by declareJackDead on disconnect.
    return handshakeWirelessManager.getMacPeer(port) != nullptr
        ? PortStatus::CONNECTED
        : PortStatus::DISCONNECTED;
}

void RemoteDeviceCoordinator::setOnDirectPeerChange(DirectPeerChangeCallback cb) {
    onDirectPeerChange_ = std::move(cb);
}

void RemoteDeviceCoordinator::fireDirectPeerChange(SerialIdentifier port,
                                                   std::optional<Peer> previous,
                                                   std::optional<Peer> current) {
    if (onDirectPeerChange_) onDirectPeerChange_(port, previous, current);
}

DeviceType RemoteDeviceCoordinator::getPeerDeviceType(SerialIdentifier port) const {
    const ::Peer* macPeer = handshakeWirelessManager.getMacPeer(port);
    return macPeer ? macPeer->deviceType : DeviceType::UNKNOWN;
}

const uint8_t* RemoteDeviceCoordinator::getPeerMac(SerialIdentifier port) const {
    const ::Peer* peer = handshakeWirelessManager.getMacPeer(port);
    return peer ? peer->macAddr.data() : nullptr;
}

bool RemoteDeviceCoordinator::isDirectPeer(const uint8_t* mac) const {
    if (!mac) return false;
    for (SerialIdentifier port : {SerialIdentifier::INPUT_JACK, SerialIdentifier::OUTPUT_JACK}) {
        const uint8_t* peer = getPeerMac(port);
        if (peer && memcmp(peer, mac, 6) == 0) return true;
    }
    return false;
}

void RemoteDeviceCoordinator::registerPeer(const uint8_t* macAddress) {
    if (wirelessManager_ != nullptr) {
        wirelessManager_->addEspNowPeer(macAddress);
    }
}

void RemoteDeviceCoordinator::declareJackDead(SerialIdentifier jack, Device* PDN) {
    const uint8_t idx = portIndex(jack);
    // Clear the direct-peer slot BEFORE firing the callback so subscribers
    // re-querying getPeerMac() observe the slot as cleared. reconcileSelfPeers()
    // on the next sync() picks up the now-cleared macPeer, drops the edge from
    // the peer-graph, and broadcasts the updated BEACON around the ring.
    handshakeWirelessManager.removeMacPeer(jack);
    HandshakeApp* app = (jack == SerialIdentifier::INPUT_JACK)
        ? inputPortHandshake : outputPortHandshake;
    if (app) app->resetDemuxer();
    // Synthetic disconnect to game-layer subscribers, mirroring the cable-yank
    // path. DeviceType is UNKNOWN because RDC doesn't preserve it across drops.
    auto& prev = previousDirectPeer_[idx];
    if (prev.has_value()) {
        std::array<uint8_t, 6> dropped = *prev;
        fireDirectPeerChange(jack, Peer{dropped, DeviceType::UNKNOWN}, std::nullopt);
        prev = std::nullopt;
        // Release the ESP-NOW peer slot unless the same device is still our
        // direct peer on the other jack (a 2-device loop wired into both jacks).
        // The 20-slot table is finite; without this it leaks one entry per
        // distinct neighbour that silent-dies over a multi-hour event, eventually
        // rejecting new peers and silently failing matches. macPeer for this jack
        // was already cleared above, so isDirectPeer() now only sees the other.
        if (!isDirectPeer(dropped.data())) {
            unregisterPeer(dropped.data());
        }
    }
    stats_[idx].jacksDeclaredDead++;
    // Reset the silent-link baseline so a fresh CONNECTED edge re-seeds it.
    lastHelloRxMs_[idx] = 0;
}

void RemoteDeviceCoordinator::unregisterPeer(const uint8_t* macAddress) {
    if (wirelessManager_ != nullptr) {
        wirelessManager_->removeEspNowPeer(macAddress);
    }
    for (SerialIdentifier port : {SerialIdentifier::INPUT_JACK, SerialIdentifier::OUTPUT_JACK}) {
        const ::Peer* peer = handshakeWirelessManager.getMacPeer(port);
        if (peer != nullptr && memcmp(peer->macAddr.data(), macAddress, 6) == 0) {
            handshakeWirelessManager.removeMacPeer(port);
        }
    }
}

