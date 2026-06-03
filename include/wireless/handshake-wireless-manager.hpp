#pragma once

//
// Created by Elli Furedy on 2/22/2025.
//
#include <array>
#include <cstdint>
#include <map>
#include "device/drivers/serial-wrapper.hpp"
#include "game/player.hpp"
#include "mac-functions.hpp"
#include "device/wireless-manager.hpp"
#include "device/serial-manager.hpp"
#include "device/device-type.hpp"

struct Peer {
    std::array<uint8_t, 6> macAddr;
    SerialIdentifier sid;
    DeviceType deviceType;
};

class HandshakeWirelessManager {
public:
    HandshakeWirelessManager();
    ~HandshakeWirelessManager();

    void initialize(WirelessManager* wirelessManager);

    // Registers a direct peer on a jack. Returns false if the peer's MAC
    // equals our own (self-loopback or spoofing) — the peer is not stored
    // and the caller must not proceed with the handshake.
    bool setMacPeer(SerialIdentifier jack, Peer peer);

    void removeMacPeer(SerialIdentifier jack);

    const Peer* getMacPeer(SerialIdentifier jack) const;

private:
    WirelessManager* wirelessManager;

    std::map<SerialIdentifier, Peer> macPeers;
};
