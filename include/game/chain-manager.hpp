#pragma once

#include <algorithm>
#include <array>
#include <functional>
#include <optional>
#include <vector>
#include <cstdint>
#include <cstring>
#include "game/player.hpp"
#include "device/remote-device-coordinator.hpp"
#include "device/wireless-manager.hpp"
#include "wireless/peer-comms-types.hpp"
#include "wireless/resender.hpp"
#include "wireless/wireless-transport.hpp"
#include "device/drivers/serial-wrapper.hpp"

enum class ChainGameEventType : uint8_t {
    COUNTDOWN = 0,
    DRAW = 1,
    WIN = 2,
    LOSS = 3,
};

// seqId = 0 is a sentinel meaning "no ACK expected, no retry". Used for
// COUNTDOWN and DRAW which are time-critical and must not arrive late.
// WIN and LOSS use a nonzero seqId and are retransmitted until ACKed or
// the retry budget is exhausted.

class ChainManager {
public:
    ChainManager(Player* player, WirelessManager* wirelessManager, RemoteDeviceCoordinator* rdc);
    virtual ~ChainManager() = default;

    bool isChampion() const;
    bool isSupporter() const;
    // True on the device that claimed coordinator after observing loop closure.
    virtual bool isCoordinator() const { return isCoordinator_; }
    // True when the local roster is stable AND observes a closed loop. Every
    // ring member sees this independently — used by Quickdraw to drive each
    // device into ShootoutProposal on loop closure (the spec calls for every
    // participant to be prompted simultaneously, not just the coordinator).
    virtual bool isInStableLoop() const {
        return rdc_ != nullptr && rdc_->isTopologyStable() && rdc_->isInLoop();
    }
    // Passthrough so state classes holding a ChainManager* can gate on a
    // settled topology without their own rdc reference. Virtual so tests can
    // stub it alongside isInStableLoop().
    virtual bool isTopologyStable() const {
        return rdc_ != nullptr && rdc_->isTopologyStable();
    }
    // The roster has SETTLED into a non-loop. The stability term is essential:
    // !isInStableLoop() alone is also true mid-churn, so gating on it without
    // isTopologyStable() would abort a live tournament on a transient blip. A
    // quick unplug->replug that never settles as a chain stays held. Composes
    // the two virtuals above so test stubs of either drive this correctly.
    bool isRingSettledOpen() const {
        return isTopologyStable() && !isInStableLoop();
    }
    bool canInitiateMatch() const;
    std::vector<std::array<uint8_t, 6>> getSupporterChainPeers() const;

    // Jack on which a same-role opponent would land. OUTPUT_JACK for hunters,
    // INPUT_JACK for bounties. ShootoutManager reads this to find the
    // loop-closing peer regardless of coordinator role.
    SerialIdentifier opponentJack() const;

    const std::vector<std::array<uint8_t, 6>>& getConfirmedSupporters() const {
        return confirmedSupporters_;
    }

    // Promote this device to coordinator. Sets isCoordinator_. Idempotent.
    // Symmetric with demoteCoordinator().
    void claimCoordinator();

    // Demote this device from coordinator. Called by ShootoutManager when a
    // competing BRACKET_ENTRY arrives from a lower-mac peer (tiebreaker for
    // simultaneous ring closure on two cables).
    void demoteCoordinator();

    void sendGameEventToSupporters(ChainGameEventType eventType);
    void sendConfirm();

    // Hook invoked when a kChainGameEvent arrives from a known supporter-chain
    // sender. Quickdraw wires this to route the event into the SupporterReady
    // state. The channel auto-acks before this fires; observers do not need to
    // emit acks themselves.
    using GameEventObserver = std::function<void(uint8_t eventType, const uint8_t* fromMac)>;
    void setGameEventObserver(GameEventObserver cb) { gameEventObserver_ = std::move(cb); }

    bool isKnownGameEventSender(const uint8_t* fromMac) const;
    void onConfirmReceived(
        const uint8_t* fromMac,
        const uint8_t* originatorMac,
        uint8_t seqId);
    void onChainStateChanged();

    // Records a peer's role learned from an incoming kRoleAnnounce packet.
    void setPeerRole(SerialIdentifier port, bool isHunter);

    unsigned long getBoostMs() const;
    size_t getConfirmedSupporterCount() const;

    // Live supporter-side chain length from topology (see chain-manager.cpp).
    // What the champion's idle screen shows before a duel countdown begins.
    size_t getChainLength() const;

    void clearSupporterConfirms();

    const uint8_t* getChampionMac() const;
    void onRoleAnnounceReceived(
        const uint8_t* fromMac,
        uint8_t role,
        const uint8_t* championMac,
        uint8_t seqId);
    void broadcastRoleAndChampion();
    void sendRoleToOpponentJack();
    void sync();

    // Subscribes the per-channel dispatchers on the supplied transport.
    // Must be called once before any receive or send traffic.
    void initialize(WirelessTransport* transport);

    // Fan-in slot for RDC's onDirectPeerChange. On disconnect, demotes
    // coordinator. Connect events do not drive coord election; derivation
    // runs continuously from the RDC roster inside sync().
    void onDirectPeerChange(SerialIdentifier port,
                            std::optional<RemoteDeviceCoordinator::Peer> previous,
                            std::optional<RemoteDeviceCoordinator::Peer> current);

    static constexpr unsigned long BOOST_PER_SUPPORTER_MS = 15;

    using RetryStats = Resender::Stats;
    // Sum across the per-PktType counters this manager owns. The Quickdraw
    // STATS log line reads this for a single rolling view of retry health.
    RetryStats getRetryStats() const {
        if (transport_ == nullptr) return RetryStats{};
        auto a = transport_->getStats(PktType::kRoleAnnounce);
        auto b = transport_->getStats(PktType::kChainGameEvent);
        auto c = transport_->getStats(PktType::kChainConfirm);
        RetryStats out{};
        out.sends = a.sends + b.sends + c.sends;
        out.retries = a.retries + b.retries + c.retries;
        out.abandons = a.abandons + b.abandons + c.abandons;
        out.ackCount = a.ackCount + b.ackCount + c.ackCount;
        out.ackLatencyMsSum = a.ackLatencyMsSum + b.ackLatencyMsSum + c.ackLatencyMsSum;
        return out;
    }
    RetryStats getRetryStats(PktType type) const {
        return transport_ ? transport_->getStats(type) : RetryStats{};
    }

private:
    Player* player_;
    WirelessManager* wirelessManager_;
    RemoteDeviceCoordinator* rdc_;

    SerialIdentifier supporterJack() const;

    // Returns the cached role of the direct peer on `port`, or nullopt if no
    // role announcement has been received from the current direct peer.
    std::optional<bool> peerIsHunter(SerialIdentifier port) const;

    std::vector<std::array<uint8_t, 6>> confirmedSupporters_;

    bool isCoordinator_ = false;

    // Per-port direct peer role; cleared when the direct peer disconnects.
    std::array<std::optional<bool>, 2> peerRoleByPort_;

    WirelessTransport* transport_ = nullptr;
    ReliableChannel<ChainGameEventPayload>* gameEventChannel_ = nullptr;
    ReliableChannel<ChainConfirmPayload>* confirmChannel_ = nullptr;
    ReliableChannel<RoleAnnouncePayload>* roleAnnounceChannel_ = nullptr;
    GameEventObserver gameEventObserver_;
    void onGameEventReceived(const uint8_t* fromMac, const ChainGameEventPayload& p);

    std::optional<std::array<uint8_t, 6>> championMac_;
    std::optional<std::array<uint8_t, 6>> lastAnnouncedSupporterJackMac_;
    std::optional<std::array<uint8_t, 6>> lastAnnouncedOpponentJackMac_;

    // Coord-eligibility derivation state. Updated once per ~1Hz sync() cycle.
    // lastStableMin_ holds the previously-observed lowest MAC in
    // getChainMembers(); stableMinCycles_ counts consecutive cycles the
    // current min has matched. Claim is gated on stableMinCycles_ >= 1, a
    // 1-cycle stability window before claiming coordinator.
    std::optional<std::array<uint8_t, 6>> lastStableMin_;
    int stableMinCycles_ = 0;
    // Dedup key for the deriveCoordinator diagnostic so the per-tick log (and
    // its MAC-string build) only fires when the decision inputs change, not at
    // the 1Hz backstop cadence.
    uint32_t lastCoordLogKey_ = 0xFFFFFFFFu;
    unsigned long nextMinStabilityCheckMs_ = 0;
    static constexpr unsigned long kCoordStabilityCycleMs = 1000;

    // Periodic, idempotent role re-announce to current direct jack peers over
    // ESP-NOW. The event-driven announces in onChainStateChanged fire once on a
    // peer-MAC change and are lost when the ESP-NOW role packet beats the
    // receiver's serial handshake (it arrives before macPeer is set and is
    // dropped as 'from a stranger'). This 1Hz refresh closes the race: once
    // both sides settle, the next refresh lands. Game-layer concern on the game
    // transport; mirrors the device-layer BEACON backstop, not folded into it.
    void reannounceRoleToPeers();
    unsigned long nextRoleBackstopMs_ = 0;
    static constexpr unsigned long kRoleBackstopMs = 1000;

    void deriveCoordinator();
    unsigned long nowMs() const;
};
