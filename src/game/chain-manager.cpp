#include "game/chain-manager.hpp"
#include "device/drivers/logger.hpp"
#include "utils/simple-timer.hpp"

#define TAG "CHAIN"

ChainManager::ChainManager(Player* player, WirelessManager* wirelessManager, RemoteDeviceCoordinator* rdc)
    : player_(player),
      wirelessManager_(wirelessManager),
      rdc_(rdc) {}

SerialIdentifier ChainManager::opponentJack() const {
    return player_->isHunter() ? SerialIdentifier::OUTPUT_JACK : SerialIdentifier::INPUT_JACK;
}

SerialIdentifier ChainManager::supporterJack() const {
    return player_->isHunter() ? SerialIdentifier::INPUT_JACK : SerialIdentifier::OUTPUT_JACK;
}

std::optional<bool> ChainManager::peerIsHunter(SerialIdentifier port) const {
    return peerRoleByPort_[port == SerialIdentifier::INPUT_JACK ? 0 : 1];
}

void ChainManager::claimCoordinator() {
    if (isCoordinator_) return;
    isCoordinator_ = true;
    LOG_W(TAG, "claimed coordinator");
}

void ChainManager::demoteCoordinator() {
    if (!isCoordinator_) return;
    isCoordinator_ = false;
    LOG_W(TAG, "demoted from coordinator");
}

bool ChainManager::isSupporter() const {
    // A supporter has a same-role peer on its opponent jack. The loop case
    // does not need excluding here: the Idle state machine checks the
    // shootout transition (isInStableLoop) before the supporter transition,
    // so first-match-wins keeps a ring member out of the supporter path.
    auto opponentRole = peerIsHunter(opponentJack());
    if (!opponentRole.has_value()) return false;
    return *opponentRole == player_->isHunter();
}

bool ChainManager::isChampion() const {
    // Default-champion: head of own-role chain. Demoted to supporter only
    // when a same-role peer sits on our opponent jack. Coordinators (the
    // device that observed the ring closing) are neither.
    return !isSupporter() && !isCoordinator_;
}

bool ChainManager::canInitiateMatch() const {
    if (!player_->isHunter()) return false;
    if (rdc_ == nullptr) return false;
    // Act only on a settled topology, and never start a 1v1 inside a ring.
    // !isInLoop() is the one place precedence must be restated: match init is a
    // side-effecting radio send invoked imperatively from Idle, not an orderable
    // state transition, so it cannot rely on first-match-wins to defer to the
    // shootout the way the Idle->ShootoutProposal edge does.
    if (!rdc_->isTopologyStable()) return false;
    if (rdc_->isInLoop()) return false;
    auto opponentRole = peerIsHunter(opponentJack());
    if (!opponentRole.has_value()) return false;
    return *opponentRole != player_->isHunter();
}

std::vector<std::array<uint8_t, 6>> ChainManager::getSupporterChainPeers() const {
    // Coordinator never broadcasts chain duel events (it's in shootout mode).
    if (isCoordinator_) return {};
    std::vector<std::array<uint8_t, 6>> out;
    // Direct supporter-jack peer is the next hop down the chain.
    const uint8_t* direct = rdc_->getPeerMac(supporterJack());
    if (direct != nullptr) {
        std::array<uint8_t, 6> arr;
        std::memcpy(arr.data(), direct, 6);
        out.push_back(arr);
    }
    // confirmedSupporters_ holds every supporter that has confirmed up the
    // chain via kChainConfirm. Champions target them directly over ESP-NOW
    // (each supporter is already registered as a peer through the
    // RoleAnnounce cascade). Skip duplicates of the direct supporter-jack
    // peer to avoid double-sending.
    for (const auto& sup : confirmedSupporters_) {
        bool isDirect = (direct != nullptr && memcmp(sup.data(), direct, 6) == 0);
        if (!isDirect) out.push_back(sup);
    }
    return out;
}

bool ChainManager::isKnownGameEventSender(const uint8_t* fromMac) const {
    // Only the direct opponent-jack peer (our chain parent) forwards game
    // events to us. Strangers and indirect senders are dropped.
    const uint8_t* direct = rdc_->getPeerMac(opponentJack());
    if (direct != nullptr && memcmp(direct, fromMac, 6) == 0) return true;
    // The champion may also reach us directly (it has us as an ESP-NOW peer
    // via the RoleAnnounce cascade). Accept its sends as authoritative.
    if (championMac_.has_value() &&
        memcmp(championMac_->data(), fromMac, 6) == 0) return true;
    return false;
}

void ChainManager::sendGameEventToSupporters(ChainGameEventType eventType) {
    if (!isChampion()) return;
    if (gameEventChannel_ == nullptr) return;

    if (eventType == ChainGameEventType::COUNTDOWN) {
        clearSupporterConfirms();
    }

    // WIN/LOSS are state-terminal for the supporter UI and must arrive or
    // the supporter display sticks on a stale screen until the next chain
    // event. They get seqIds and retry tracking.
    //
    // COUNTDOWN/DRAW are time-sensitive transient events. A late-arriving
    // retry of COUNTDOWN would falsely re-arm a supporter whose duel has
    // already resolved; a late DRAW would disarm a supporter who just
    // entered a new COUNTDOWN. Keep them fire-and-forget: seqId=0.
    bool wantsAck = (eventType == ChainGameEventType::WIN ||
                     eventType == ChainGameEventType::LOSS);

    auto peers = getSupporterChainPeers();
    for (const auto& peerMac : peers) {
        ChainGameEventPayload payload{};
        payload.event_type = static_cast<uint8_t>(eventType);
        payload.seqId = 0;
        if (wantsAck) {
            gameEventChannel_->sendReliable(peerMac.data(), payload);
        } else {
            gameEventChannel_->sendOnce(peerMac.data(), payload);
        }
    }
}

void ChainManager::sendConfirm() {
    if (!championMac_.has_value()) {
        LOG_W(TAG, "sendConfirm SKIP: no championMac");
        return;
    }
    if (confirmChannel_ == nullptr) return;

    const uint8_t* selfMac = wirelessManager_->getMacAddress();
    if (selfMac == nullptr) return;

    // The champion's own championMac_ is self; confirming would unicast to self
    // and inflate its supporter count. Only supporters confirm upstream.
    if (memcmp(championMac_->data(), selfMac, 6) == 0) return;

    LOG_W(TAG, "sendConfirm -> %02X:%02X:%02X:%02X:%02X:%02X",
          (*championMac_)[0], (*championMac_)[1], (*championMac_)[2],
          (*championMac_)[3], (*championMac_)[4], (*championMac_)[5]);
    ChainConfirmPayload payload{};
    memcpy(payload.originatorMac, selfMac, 6);
    confirmChannel_->sendReliable(championMac_->data(), payload);
}

void ChainManager::onConfirmReceived(
    const uint8_t* fromMac,
    const uint8_t* originatorMac,
    uint8_t seqId) {
    (void)fromMac; (void)seqId;

    // Universal topology-stability gate: drop confirms that arrive
    // mid-convergence. BEACON propagation settles isTopologyStable within a few
    // hundred ms of the last topology change on both linear chains and rings, so
    // the gate is fast enough to apply uniformly without holding 2-device posse
    // formation. The sender's resender retries until the gate opens.
    if (rdc_ != nullptr && !rdc_->isTopologyStable()) {
        LOG_W(TAG, "onConfirmReceived dropped (topology unstable)");
        return;
    }

    // Topology-membership integrity. The channel is direct unicast keyed by our
    // MAC, so reaching this handler means some device resolved us as its
    // championMac. Accept only if that originator is one we can actually see in
    // our topology: either our direct supporter-jack peer (legitimate even
    // before its BEACON has propagated into our graph) or a member of our
    // connected component. getChainMembers() is a BFS over mutual BEACON edges,
    // so it covers multi-hop supporters in both linear chains and rings -- the
    // ring coordinator still accumulates every member's confirm to detect loop
    // closure. Without this gate a device holding a stale championMac_ (after a
    // topology reshuffle) could unicast a confirm and inflate the boost.
    if (rdc_ != nullptr) {
        bool isMember = false;
        const uint8_t* directSupporter = rdc_->getPeerMac(supporterJack());
        if (directSupporter != nullptr &&
            memcmp(directSupporter, originatorMac, 6) == 0) {
            isMember = true;
        }
        if (!isMember) {
            for (const auto& m : rdc_->getChainMembers()) {
                if (memcmp(m.data(), originatorMac, 6) == 0) { isMember = true; break; }
            }
        }
        if (!isMember) {
            LOG_W(TAG, "onConfirmReceived dropped (originator not in topology)");
            return;
        }
    }

    // Dedup on originator MAC.
    for (const auto& existing : confirmedSupporters_) {
        if (memcmp(existing.data(), originatorMac, 6) == 0) {
            LOG_W(TAG, "onConfirmReceived dedupe (already confirmed)");
            return;
        }
    }
    std::array<uint8_t, 6> macArr;
    memcpy(macArr.data(), originatorMac, 6);
    confirmedSupporters_.push_back(macArr);
    LOG_W(TAG, "onConfirmReceived ACCEPTED from=%02X:%02X:%02X:%02X:%02X:%02X newCount=%u",
          originatorMac[0], originatorMac[1], originatorMac[2],
          originatorMac[3], originatorMac[4], originatorMac[5],
          (unsigned)confirmedSupporters_.size());

}

void ChainManager::onChainStateChanged() {
    // Boost-clear invariant: a confirmed supporter only counts while we still
    // hold the supporter-jack direct peer it confirmed through. That peer's
    // disappearance is the only locally-observable signal the chain shrank, so
    // drain when it's gone. Level-triggered (the clear is idempotent), so no
    // prior-count state is needed to detect the transition.
    const bool supporterPeerPresent = (rdc_ != nullptr && rdc_->getPeerMac(supporterJack()) != nullptr);
    if (!supporterPeerPresent && !confirmedSupporters_.empty()) {
        LOG_W(TAG, "supporter-jack peer gone, draining %u confirmed supporters",
              (unsigned)confirmedSupporters_.size());
        clearSupporterConfirms();
    }

    // Drop cached peer roles for any port that no longer has a direct peer.
    // Immediate clearing is safe because the cross-jack re-broadcast in
    // onRoleAnnounceReceived recovers the race with in-flight RoleAnnounces
    // that arrive before the handshake completes (when a peer's RoleAnnounce
    // arrives on our supporter-jack side, we re-trigger our own broadcast),
    // and it gives prompt UI feedback on cable yank.
    for (SerialIdentifier port : {SerialIdentifier::INPUT_JACK, SerialIdentifier::OUTPUT_JACK}) {
        if (rdc_->getPeerMac(port) == nullptr) {
            peerRoleByPort_[port == SerialIdentifier::INPUT_JACK ? 0 : 1].reset();
        }
    }

    // Re-evaluate championMac_. If I'm now champion, self-assign.
    if (isChampion()) {
        const uint8_t* selfMac = wirelessManager_->getMacAddress();
        if (selfMac != nullptr) {
            std::array<uint8_t, 6> selfArr;
            memcpy(selfArr.data(), selfMac, 6);
            if (!championMac_.has_value() || *championMac_ != selfArr) {
                championMac_ = selfArr;
                broadcastRoleAndChampion();
                // Track that we just announced to our current supporter-jack peer.
                const uint8_t* supporterPeer = rdc_->getPeerMac(supporterJack());
                if (supporterPeer != nullptr) {
                    std::array<uint8_t, 6> cur;
                    memcpy(cur.data(), supporterPeer, 6);
                    lastAnnouncedSupporterJackMac_ = cur;
                }
                // Announce role to opponent-jack peer if new.
                const uint8_t* opponentPeer = rdc_->getPeerMac(opponentJack());
                if (opponentPeer != nullptr) {
                    std::array<uint8_t, 6> cur;
                    memcpy(cur.data(), opponentPeer, 6);
                    if (!lastAnnouncedOpponentJackMac_.has_value() || *lastAnnouncedOpponentJackMac_ != cur) {
                        lastAnnouncedOpponentJackMac_ = cur;
                        sendRoleToOpponentJack();
                    }
                }
                return;
            }
        }
    }

    // If we've become a supporter but still hold our own MAC as champion,
    // invalidate it — the real champion's announce will repopulate via
    // onRoleAnnounceReceived.
    if (isSupporter() && championMac_.has_value()) {
        const uint8_t* selfMac = wirelessManager_->getMacAddress();
        if (selfMac != nullptr && memcmp(championMac_->data(), selfMac, 6) == 0) {
            championMac_.reset();
        }
    }

    // If a new supporter-jack direct peer has appeared since our last announce,
    // broadcast the current championMac to it. Guard against re-firing on every
    // chain-state event (which would cause packet storms).
    const uint8_t* supporterPeer = rdc_->getPeerMac(supporterJack());
    if (championMac_.has_value() && supporterPeer != nullptr) {
        std::array<uint8_t, 6> cur;
        memcpy(cur.data(), supporterPeer, 6);
        if (!lastAnnouncedSupporterJackMac_.has_value() || *lastAnnouncedSupporterJackMac_ != cur) {
            lastAnnouncedSupporterJackMac_ = cur;
            broadcastRoleAndChampion();
        }
    } else if (supporterPeer == nullptr) {
        lastAnnouncedSupporterJackMac_.reset();
    }

    // Announce our role to the opponent-jack peer if it has changed.
    const uint8_t* opponentPeer = rdc_->getPeerMac(opponentJack());
    if (opponentPeer != nullptr) {
        std::array<uint8_t, 6> cur;
        memcpy(cur.data(), opponentPeer, 6);
        if (!lastAnnouncedOpponentJackMac_.has_value() || *lastAnnouncedOpponentJackMac_ != cur) {
            lastAnnouncedOpponentJackMac_ = cur;
            sendRoleToOpponentJack();
        }
    } else {
        lastAnnouncedOpponentJackMac_.reset();
    }
}

void ChainManager::setPeerRole(SerialIdentifier port, bool isHunter) {
    peerRoleByPort_[port == SerialIdentifier::INPUT_JACK ? 0 : 1] = isHunter;
}

unsigned long ChainManager::getBoostMs() const {
    return confirmedSupporters_.size() * BOOST_PER_SUPPORTER_MS;
}

size_t ChainManager::getConfirmedSupporterCount() const {
    return confirmedSupporters_.size();
}

size_t ChainManager::getChainLength() const {
    // Live count of supporters chained behind us on the supporter-jack side,
    // derived from topology — distinct from confirmedSupporters_, which only
    // fills as supporters confirm during a duel countdown. The posse belongs
    // to the champion: a mid-chain supporter has its own downstream peers but
    // is not leading them, so it reports zero.
    if (rdc_ == nullptr || !isChampion()) return 0;
    return rdc_->countChainBehind(supporterJack());
}

void ChainManager::clearSupporterConfirms() {
    confirmedSupporters_.clear();
}

const uint8_t* ChainManager::getChampionMac() const {
    return championMac_.has_value() ? championMac_->data() : nullptr;
}

void ChainManager::onRoleAnnounceReceived(
    const uint8_t* fromMac,
    uint8_t role,
    const uint8_t* championMac,
    uint8_t seqId) {
    LOG_W(TAG, "RoleAnnounce rx from=%02X:%02X role=%i selfHunter=%i opp=%i",
          fromMac[4], fromMac[5], (int)role,
          player_->isHunter() ? 1 : 0,
          static_cast<int>(opponentJack()));
    // 1. Update peer role based on which jack fromMac is on.
    //    Also remember whether this announce came from our opponent-jack
    //    (parent) direction — only opponent-jack announces authoritatively
    //    update our championMac_ cache.
    bool fromOpponentJack = false;
    bool fromKnownDirectPeer = false;
    for (SerialIdentifier port : {SerialIdentifier::INPUT_JACK, SerialIdentifier::OUTPUT_JACK}) {
        const uint8_t* directMac = rdc_->getPeerMac(port);
        if (directMac != nullptr && memcmp(directMac, fromMac, 6) == 0) {
            setPeerRole(port, role == 1);
            fromKnownDirectPeer = true;
            if (port == opponentJack()) {
                fromOpponentJack = true;
            }
            break;
        }
    }

    // 2. Only act on announces from known direct peers. Strangers in radio
    //    range are dropped before any state mutation. The channel's deliver
    //    path already emitted an auto-ack for any seqId != 0 payload, so the
    //    drop here is logical-only.
    if (!fromKnownDirectPeer) return;
    (void)seqId;

    // 3 & 4. Only same-role opponent-jack announces authoritatively update
    // championMac_. Opposite-role senders are dueling opponents, not chain
    // parents — their championMac is irrelevant.
    // Before returning, re-arm the supporter-jack announcement tracker: when a
    // peer's RoleAnnounce arrives on our supporter-jack side (e.g., they just
    // finished their own handshake and sent sendRoleToOpponentJack), our
    // original broadcast to them may have arrived before their handshake
    // completed and been dropped. Re-broadcasting now ensures they get our
    // championMac after they're ready to process it. Without this, the race
    // strands the supporter without a champion to confirm against.
    if (!fromOpponentJack) {
        if (rdc_ != nullptr) {
            const uint8_t* supporterPeer = rdc_->getPeerMac(supporterJack());
            // If the announce came from our supporter-jack peer, re-fire our
            // own announce ONCE to recover from the handshake-race where our
            // first broadcast arrived before they were ready. Guard on
            // lastAnnouncedSupporterJackMac_: if we've already announced to
            // this exact peer, do NOT reset — otherwise every inbound announce
            // forces an outbound, the peer answers in kind, and the two devices
            // ping-pong RoleAnnounce at tens-per-second indefinitely.
            if (supporterPeer != nullptr &&
                memcmp(supporterPeer, fromMac, 6) == 0) {
                std::array<uint8_t, 6> cur;
                memcpy(cur.data(), supporterPeer, 6);
                if (!lastAnnouncedSupporterJackMac_.has_value() ||
                    *lastAnnouncedSupporterJackMac_ != cur) {
                    lastAnnouncedSupporterJackMac_.reset();
                }
            }
        }
        onChainStateChanged();
        return;
    }
    if (role != (player_->isHunter() ? 1u : 0u)) return;

    // 3. Register champion as ESP-NOW peer (only if it's not our own MAC).
    const uint8_t* selfMac = wirelessManager_->getMacAddress();
    bool championIsSelf = (selfMac != nullptr &&
                           memcmp(selfMac, championMac, 6) == 0);
    if (!championIsSelf) {
        rdc_->registerPeer(championMac);
    }

    // 4. Update championMac_ and cascade if changed. On change, release the
    // ESP-NOW peer slot held by the OLD champion MAC unless it's still
    // reachable via one of our jacks (then its registration is owned by the
    // chain-peer bookkeeping and will be cleaned up when that list changes).
    std::array<uint8_t, 6> newMac;
    memcpy(newMac.data(), championMac, 6);
    bool changed = !championMac_.has_value() || *championMac_ != newMac;
    if (changed && championMac_.has_value()) {
        std::array<uint8_t, 6> oldMac = *championMac_;
        bool oldIsSelf = (selfMac != nullptr &&
                          memcmp(selfMac, oldMac.data(), 6) == 0);
        if (!oldIsSelf) {
            // Reachability collapses to "direct peer on one of our jacks":
            // if the displaced champion isn't a current direct peer, drop
            // its ESP-NOW slot. RoleAnnounce cascades from the new neighbor
            // will re-register it if it still belongs.
            bool oldStillDirect = (rdc_ && rdc_->isDirectPeer(oldMac.data()));
            if (!oldStillDirect) {
                rdc_->unregisterPeer(oldMac.data());
            }
        }
    }
    championMac_ = newMac;
    if (changed) {
        broadcastRoleAndChampion();
        // Auto-announce supporter membership. Without the daisy view, the
        // would-be coordinator can only learn about us via this ChainConfirm
        // — the press-to-confirm UI in SupporterReady serves as user intent
        // for a duel, not as the protocol's loop-detection signal. We must
        // confirm any time championMac_ resolves to a non-self peer, even
        // before the user has interacted.
        if (selfMac != nullptr && !championIsSelf) {
            sendConfirm();
        }
    }
}

void ChainManager::broadcastRoleAndChampion() {
    if (!championMac_.has_value()) return;
    if (roleAnnounceChannel_ == nullptr) return;

    const uint8_t* supporterPeer = rdc_->getPeerMac(supporterJack());
    if (supporterPeer == nullptr) return;

    RoleAnnouncePayload payload{};
    payload.role = player_->isHunter() ? 1 : 0;
    memcpy(payload.championMac, championMac_->data(), 6);
    roleAnnounceChannel_->sendReliable(supporterPeer, payload);
}

// Fire-and-forget, asymmetric with broadcastRoleAndChampion which has
// ACK+retry on the supporter-jack direction. Rationale: losing this packet
// leaves the opponent momentarily uncertain of our role, but their own
// onChainStateChanged will fire whenever their topology shifts (including
// our handshake completion) and trigger a fresh broadcast toward us. So the
// system is self-healing on any real topology change. Adding ACK+retry here
// would double the airtime cost of every chain change; the relaxed model is
// acceptable given the healing path.
void ChainManager::sendRoleToOpponentJack() {
    if (roleAnnounceChannel_ == nullptr) return;
    const uint8_t* opponentPeer = rdc_->getPeerMac(opponentJack());
    if (opponentPeer == nullptr) return;

    RoleAnnouncePayload payload{};
    payload.role = player_->isHunter() ? 1 : 0;
    // championMac is a placeholder; receiver ignores unless same-role peer.
    if (championMac_.has_value()) {
        memcpy(payload.championMac, championMac_->data(), 6);
    }
    // seqId=0 sentinel: fire-and-forget, no ack/retransmit. Avoids
    // burning the supporter-jack seqId counter on opponent-jack sends.
    payload.seqId = 0;
    roleAnnounceChannel_->sendOnce(opponentPeer, payload);
}

void ChainManager::sync() {
    // Coord-eligibility derivation runs at ~1Hz (matches the BEACON cadence).
    unsigned long now = nowMs();
    if (now >= nextMinStabilityCheckMs_) {
        nextMinStabilityCheckMs_ = now + kCoordStabilityCycleMs;
        deriveCoordinator();
    }
    if (now >= nextRoleBackstopMs_) {
        nextRoleBackstopMs_ = now + kRoleBackstopMs;
        reannounceRoleToPeers();
        // Self-healing confirm backstop, mirroring the role re-announce above.
        // The auto-confirm fired when championMac_ first resolved may have been
        // dropped by the champion's topology-stability gate and then abandoned
        // by the resender, all before the graph settled — and championMac_ never
        // changes again, so without this periodic re-send a supporter would
        // silently contribute zero boost. The champion dedups by originator MAC
        // and rejects non-members, so a steady repeat carries no storm.
        if (isSupporter() && championMac_.has_value()) {
            sendConfirm();
        }
    }
}

void ChainManager::reannounceRoleToPeers() {
    if (rdc_ == nullptr) return;
    // Both sends self-guard on peer presence (and championMac_ for the
    // supporter direction), so unconditional calls are safe. Receivers apply
    // setPeerRole idempotently and only cascade champion changes, so a steady
    // 1Hz repeat carries no storm — it just keeps a raced-away role fresh.
    sendRoleToOpponentJack();
    broadcastRoleAndChampion();
}

unsigned long ChainManager::nowMs() const {
    auto* clk = SimpleTimer::getPlatformClock();
    return clk ? clk->milliseconds() : 0;
}

void ChainManager::deriveCoordinator() {
    if (rdc_ == nullptr) return;

    // Universal stability gate: any consumer of isInLoop / getChainMembers
    // must guard on isTopologyStable, otherwise mid-convergence views can flip
    // a coord claim or surrender it prematurely. Hold prior state until the
    // graph settles.
    const bool stable = rdc_->isTopologyStable();
    const auto members = rdc_->getChainMembers();
    const bool inLoop = rdc_->isInLoop();
    {
        const uint32_t logKey = (static_cast<uint32_t>(members.size()) << 3)
                                | (stable ? 0x4u : 0u)
                                | (inLoop ? 0x2u : 0u)
                                | (isCoordinator_ ? 0x1u : 0u);
        if (logKey != lastCoordLogKey_) {
            lastCoordLogKey_ = logKey;
            char mbuf[256] = {0};
            size_t mp = 0;
            for (const auto& m : members) {
                if (mp < sizeof(mbuf) - 14)
                    mp += snprintf(mbuf + mp, sizeof(mbuf) - mp,
                                   "%02X%02X%02X%02X%02X%02X ",
                                   m[0], m[1], m[2], m[3], m[4], m[5]);
            }
            LOG_W(TAG, "deriveCoordinator stable=%d inLoop=%d members=%u coord=%d [%s]",
                  (int)stable, (int)inLoop, (unsigned)members.size(), (int)isCoordinator_, mbuf);
        }
    }
    if (!stable) return;

    if (members.empty() || !inLoop) {
        lastStableMin_ = std::nullopt;
        stableMinCycles_ = 0;
        if (isCoordinator_) demoteCoordinator();
        return;
    }

    auto minIt = std::min_element(
        members.begin(), members.end(),
        [](const auto& a, const auto& b) {
            return std::memcmp(a.data(), b.data(), 6) < 0;
        });
    std::array<uint8_t, 6> currentMin = *minIt;

    if (lastStableMin_.has_value() && *lastStableMin_ == currentMin) {
        if (stableMinCycles_ < 1) stableMinCycles_++;
    } else {
        // Min value changed since the last cycle. Reseed and require another
        // full cycle of stability before any eligibility flip is honored.
        lastStableMin_ = currentMin;
        stableMinCycles_ = 0;
    }

    std::array<uint8_t, 6> selfMac{};
    const uint8_t* selfPtr = wirelessManager_ ? wirelessManager_->getMacAddress() : nullptr;
    if (selfPtr == nullptr) return;
    std::memcpy(selfMac.data(), selfPtr, 6);

    if (stableMinCycles_ >= 1 && currentMin == selfMac && !isCoordinator_) {
        claimCoordinator();
    } else if (currentMin != selfMac && isCoordinator_) {
        // Someone with a strictly lower MAC has appeared. Defer to them; the
        // Shootout BRACKET_ENTRY tiebreaker is a separate safety net.
        demoteCoordinator();
    }
}

void ChainManager::initialize(WirelessTransport* transport) {
    transport_ = transport;

    gameEventChannel_ = transport_->channel<ChainGameEventPayload>(
        PktType::kChainGameEvent,
        [](uint8_t seqId, const uint8_t* target) {
            LOG_W(TAG,
                "kChainGameEvent abandoned: target=%02X:%02X:%02X:%02X:%02X:%02X seqId=%u",
                target[0], target[1], target[2], target[3], target[4], target[5],
                (unsigned)seqId);
        });
    gameEventChannel_->onReceive(
        [this](const uint8_t* fromMac, const ChainGameEventPayload& p) {
            onGameEventReceived(fromMac, p);
        });

    confirmChannel_ = transport_->channel<ChainConfirmPayload>(
        PktType::kChainConfirm,
        [](uint8_t /*seqId*/, const uint8_t* /*target*/) {});
    confirmChannel_->onReceive(
        [this](const uint8_t* fromMac, const ChainConfirmPayload& p) {
            onConfirmReceived(fromMac, p.originatorMac, p.seqId);
        });

    roleAnnounceChannel_ = transport_->channel<RoleAnnouncePayload>(
        PktType::kRoleAnnounce,
        [](uint8_t seqId, const uint8_t* target) {
            LOG_W(TAG,
                "kRoleAnnounce abandoned: target=%02X:%02X:%02X:%02X:%02X:%02X seqId=%u",
                target[0], target[1], target[2], target[3], target[4], target[5],
                (unsigned)seqId);
        });
    roleAnnounceChannel_->onReceive(
        [this](const uint8_t* fromMac, const RoleAnnouncePayload& p) {
            onRoleAnnounceReceived(fromMac, p.role, p.championMac, p.seqId);
        });

    // When the player's role finalizes after registration, the handshake has
    // typically already exchanged RoleAnnounces using the default role, leaving
    // peerRoleByPort_ caches stale on both sides. Invalidate the cached values
    // and the per-peer "last announced" tracker so the next sync re-broadcasts
    // with the fresh role and the peer (which fires the same callback) does
    // the symmetric refresh.
    if (player_ != nullptr) {
        player_->setOnRoleChanged([this]() {
            peerRoleByPort_[0].reset();
            peerRoleByPort_[1].reset();
            lastAnnouncedSupporterJackMac_.reset();
            lastAnnouncedOpponentJackMac_.reset();
            championMac_.reset();
            onChainStateChanged();
        });
    }

    // Route inbound bytes from the wireless driver into the transport's
    // per-channel dispatcher. Without these registrations each receive path
    // is dead. subType=0 is the sole sub for each of these PktTypes.
    WirelessManager* wm = transport_->getWirelessManager();
    if (wm != nullptr) {
        wm->setEspNowPacketHandler(
            PktType::kChainGameEvent,
            [](const uint8_t* fromMac, const uint8_t* data, const size_t dataLen, void* ctx) {
                auto* self = static_cast<ChainManager*>(ctx);
                if (self->transport_) {
                    self->transport_->deliverIncoming(
                        PktType::kChainGameEvent, 0, fromMac, data, dataLen);
                }
            },
            this);
        wm->setEspNowPacketHandler(
            PktType::kChainConfirm,
            [](const uint8_t* fromMac, const uint8_t* data, const size_t dataLen, void* ctx) {
                auto* self = static_cast<ChainManager*>(ctx);
                if (self->transport_) {
                    self->transport_->deliverIncoming(
                        PktType::kChainConfirm, 0, fromMac, data, dataLen);
                }
            },
            this);
        wm->setEspNowPacketHandler(
            PktType::kRoleAnnounce,
            [](const uint8_t* fromMac, const uint8_t* data, const size_t dataLen, void* ctx) {
                auto* self = static_cast<ChainManager*>(ctx);
                if (self->transport_) {
                    self->transport_->deliverIncoming(
                        PktType::kRoleAnnounce, 0, fromMac, data, dataLen);
                }
            },
            this);
    }
}

void ChainManager::onGameEventReceived(const uint8_t* fromMac,
                                       const ChainGameEventPayload& p) {
    // Sender-gate: only accept events from supporter-chain peers. Channel
    // already auto-acked the payload (if seqId != 0); the drop here is a
    // logical one so strangers can't drive supporter UI transitions.
    if (!isKnownGameEventSender(fromMac)) return;
    if (gameEventObserver_) gameEventObserver_(p.event_type, fromMac);
}

void ChainManager::onDirectPeerChange(SerialIdentifier port,
                                      std::optional<RemoteDeviceCoordinator::Peer> /*previous*/,
                                      std::optional<RemoteDeviceCoordinator::Peer> current) {
    LOG_W(TAG, "onDirectPeerChange port=%c curHasVal=%d",
          port == SerialIdentifier::INPUT_JACK ? 'I' : 'O',
          (int)current.has_value());
    if (!current.has_value()) {
        // Direct peer dropped on this jack. If we were coordinator, the loop
        // just opened on our cable — demote so ShootoutManager observes the
        // change and broadcasts ABORT to peers. deriveCoordinator() will also
        // catch this on the next sync tick via isInLoop()==false, but acting
        // immediately keeps the ABORT path sub-second.
        if (isCoordinator_) {
            demoteCoordinator();
        }
        onChainStateChanged();
        return;
    }

    onChainStateChanged();
}
