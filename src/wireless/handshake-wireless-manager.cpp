//
// Created by Elli Furedy on 2/22/2025.
//
#include <cstring>
#include "wireless/handshake-wireless-manager.hpp"

HandshakeWirelessManager::HandshakeWirelessManager() {}

HandshakeWirelessManager::~HandshakeWirelessManager() {
    wirelessManager = nullptr;
}

void HandshakeWirelessManager::initialize(WirelessManager* wirelessManager) {
    this->wirelessManager = wirelessManager;
}

bool HandshakeWirelessManager::setMacPeer(SerialIdentifier jack, Peer peer) {
    // Reject self-MAC: a peer claiming our own MAC (self-loopback or a neighbor
    // spoofing it) must never register as a direct peer — violates spec
    // invariant DirectPeerIsNeverSelf.
    if (wirelessManager != nullptr) {
        const uint8_t* selfMac = wirelessManager->getMacAddress();
        if (selfMac != nullptr && memcmp(peer.macAddr.data(), selfMac, 6) == 0) {
            return false;
        }
    }
    auto it = macPeers.find(jack);
    const bool changed = (it == macPeers.end() ||
                          memcmp(it->second.macAddr.data(), peer.macAddr.data(), 6) != 0);
    if (changed) {
        LOG_W("HWM", "setMacPeer jack=%c mac=%02X:%02X:%02X:%02X:%02X:%02X sid=%d type=%d",
              jack == SerialIdentifier::INPUT_JACK ? 'I' : 'O',
              peer.macAddr[0], peer.macAddr[1], peer.macAddr[2],
              peer.macAddr[3], peer.macAddr[4], peer.macAddr[5],
              (int)peer.sid, (int)peer.deviceType);
    }
    macPeers[jack] = peer;
    return true;
}

void HandshakeWirelessManager::removeMacPeer(SerialIdentifier jack) {
    macPeers.erase(jack);
}

const Peer* HandshakeWirelessManager::getMacPeer(SerialIdentifier jack) const {
    auto it = macPeers.find(jack);
    if (it == macPeers.end()) return nullptr;
    return &it->second;
}
