#include "game/shootout-manager.hpp"
#include "game/match-manager.hpp"
#include "device/drivers/logger.hpp"
#include "device/device-constants.hpp"
#include "utils/simple-timer.hpp"
#include "wireless/mac-functions.hpp"
#include "id-generator.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>

#define TAG "SHT"

namespace {
void deriveShootoutMatchId(int matchIndex, char* out, size_t outSize) {
    // Deterministic ID so both duelists prime MatchManager with the same
    // value without a SEND_MATCH_ID handshake.
    snprintf(out, outSize, "%s%032d", kShootoutMatchIdPrefix, matchIndex);
}
}

ShootoutManager::ShootoutManager(Player* player,
                                 WirelessManager* wirelessManager,
                                 RemoteDeviceCoordinator* rdc,
                                 ChainManager* cdm)
    : player_(player), wirelessManager_(wirelessManager), rdc_(rdc), cdm_(cdm) {}

void ShootoutManager::initialize(WirelessTransport* transport) {
    transport_ = transport;
    if (transport_ == nullptr) return;

    auto abandonToAbort = [this](uint8_t /*seqId*/, const uint8_t* /*mac*/) {
        // BRACKET_ENTRY / MATCH_START / MATCH_RESULT abandons drop tournament
        // state. Latched and processed in sync() so we don't mutate the
        // pending list from inside the Resender's callback chain.
        tournamentAbortPending_ = true;
    };
    auto abandonNoop = [](uint8_t /*seqId*/, const uint8_t* /*mac*/) {};

    // Stream channel: the bracket roster is split into one reliable packet per
    // slot, all sent to the same peer. Each slot must keep its own retry entry,
    // so a dropped non-final slot still retransmits instead of being superseded.
    bracketEntryChannel_ = transport_->channel<ShootoutBracketEntryPayload, ShootoutCmd>(
        PktType::kShootoutCommand, ShootoutCmd::BRACKET_ENTRY, abandonToAbort,
        Resender::SendMode::KeepDistinct);
    bracketEntryChannel_->onReceive(
        [this](const uint8_t* fromMac, const ShootoutBracketEntryPayload& p) {
            onBracketEntryReceived(fromMac, p);
        });

    matchStartChannel_ = transport_->channel<ShootoutMatchStartPayload, ShootoutCmd>(
        PktType::kShootoutCommand, ShootoutCmd::MATCH_START, abandonToAbort);
    matchStartChannel_->onReceive(
        [this](const uint8_t* /*fromMac*/, const ShootoutMatchStartPayload& p) {
            onMatchStartReceived(p.duelistA, p.duelistB, p.matchIndex, p.seqId);
        });

    matchResultChannel_ = transport_->channel<ShootoutMatchResultPayload, ShootoutCmd>(
        PktType::kShootoutCommand, ShootoutCmd::MATCH_RESULT, abandonToAbort);
    matchResultChannel_->onReceive(
        [this](const uint8_t* fromMac, const ShootoutMatchResultPayload& p) {
            onMatchResultReceived(p.winner, p.loser, p.matchIndex, p.seqId, fromMac);
        });

    tournamentEndChannel_ = transport_->channel<ShootoutTournamentEndPayload, ShootoutCmd>(
        PktType::kShootoutCommand, ShootoutCmd::TOURNAMENT_END, abandonNoop);
    tournamentEndChannel_->onReceive(
        [this](const uint8_t* /*fromMac*/, const ShootoutTournamentEndPayload& p) {
            onTournamentEndReceived(p.winner, p.seqId);
        });

    confirmChannel_ = transport_->channel<ShootoutConfirmPayload, ShootoutCmd>(
        PktType::kShootoutCommand, ShootoutCmd::CONFIRM, abandonNoop);
    confirmChannel_->onReceive(
        [this](const uint8_t* fromMac, const ShootoutConfirmPayload& p) {
            onConfirmReceived(p.mac, p.name);
            (void)fromMac;
        });

    peerLostChannel_ = transport_->channel<ShootoutPeerLostPayload, ShootoutCmd>(
        PktType::kShootoutCommand, ShootoutCmd::PEER_LOST, abandonNoop);
    peerLostChannel_->onReceive(
        [this](const uint8_t* /*fromMac*/, const ShootoutPeerLostPayload& p) {
            onPeerLostReceived(p.mac);
        });

    abortChannel_ = transport_->channel<ShootoutAbortPayload, ShootoutCmd>(
        PktType::kShootoutCommand, ShootoutCmd::ABORT, abandonNoop);
    abortChannel_->onReceive(
        [this](const uint8_t* /*fromMac*/, const ShootoutAbortPayload& /*p*/) {
            onAbortReceived();
        });

    // Route inbound kShootoutCommand bytes from the wireless driver into the
    // transport's per-cmd dispatcher. The leading byte of every payload is
    // the ShootoutCmd discriminator; each cmd has its own channel claim.
    WirelessManager* wm = transport_->getWirelessManager();
    if (wm != nullptr) {
        wm->setEspNowPacketHandler(
            PktType::kShootoutCommand,
            [](const uint8_t* fromMac, const uint8_t* data, const size_t dataLen, void* ctx) {
                auto* self = static_cast<ShootoutManager*>(ctx);
                if (self->transport_ == nullptr || dataLen < 1) return;
                uint8_t cmd = data[0];
                self->transport_->deliverIncoming(
                    PktType::kShootoutCommand, cmd, fromMac, data, dataLen);
            },
            this);
    }
}

bool ShootoutManager::batchIsNewer(uint8_t a, uint8_t b) {
    int8_t diff = static_cast<int8_t>(a - b);
    return diff > 0;
}

void ShootoutManager::onBracketEntryReceived(const uint8_t* fromMac,
                                             const ShootoutBracketEntryPayload& p) {
    // Competing-coordinator tiebreaker.
    // If two cables close two rings simultaneously, both devices may have
    // called claimCoordinator(). The first BRACKET_ENTRY to arrive from a
    // peer with a lower mac wins; we demote so they own the tournament.
    if (cdm_ != nullptr && cdm_->isCoordinator() && fromMac != nullptr) {
        const uint8_t* self = wirelessManager_ ? wirelessManager_->getMacAddress() : nullptr;
        if (self != nullptr && memcmp(fromMac, self, 6) < 0) {
            cdm_->demoteCoordinator();
        }
    }
    if (isCoordinator()) return;

    // totalSlots == 0 sentinel: clear local bracket view.
    if (p.totalSlots == 0) {
        pendingBracket_.active = false;
        bracket_.clear();
        currentRound_.clear();
        return;
    }

    auto& pend = pendingBracket_;
    if (!pend.active || batchIsNewer(p.batchId, pend.batchId)) {
        pend.active = true;
        pend.batchId = p.batchId;
        pend.totalSlots = p.totalSlots;
        pend.seqId = p.seqId;
        pend.buffer.assign(p.totalSlots, std::array<uint8_t, 6>{});
        pend.receivedSlots.assign(p.totalSlots, false);
    } else if (p.batchId != pend.batchId) {
        return; // stale batch; ignore.
    }

    if (p.slot >= pend.totalSlots) return;
    std::memcpy(pend.buffer[p.slot].data(), p.mac, 6);
    pend.receivedSlots[p.slot] = true;

    for (bool got : pend.receivedSlots) {
        if (!got) return;
    }

    // All slots present: atomic swap into bracket_.
    bracket_ = std::move(pend.buffer);
    currentRound_ = bracket_;
    // The coordinator is whoever sent the BRACKET_ENTRY (the device that
    // observed loop closure and built the bracket). Anchor here so the
    // post-bracket isCoordinator() check stays stable across cable nudges.
    if (fromMac != nullptr) {
        std::memcpy(coordinatorMac_.data(), fromMac, 6);
    } else {
        coordinatorMac_ = lowestMacIn(bracket_);
    }
    lastObservedBracketSeqId_ = pend.seqId;
    pend.active = false;
    phase_ = Phase::BRACKET_REVEAL;
    bracketRevealTimer_.setTimer(kBracketRevealMs);
}

bool ShootoutManager::active() const {
    return phase_ != Phase::IDLE;
}

ShootoutManager::Phase ShootoutManager::getPhase() const {
    return phase_;
}

size_t ShootoutManager::getConfirmedCount() const {
    return confirmedSet_.size();
}

std::vector<std::array<uint8_t, 6>> ShootoutManager::getBracket() const {
    return bracket_;
}

bool ShootoutManager::hasBye() const {
    return bracket_.size() % 2 == 1;
}

uint8_t ShootoutManager::getLastBracketSeqId() const {
    return lastBracketSeqId_;
}

size_t ShootoutManager::getBracketPendingAckCount() const {
    // Bracket sends are per-slot BRACKET_ENTRY through the transport.
    // Pending count is the number of slot-level in-flight entries.
    if (transport_ == nullptr) return 0;
    if (bracketEntryChannel_ == nullptr) return 0;
    // No pendingCount accessor on the transport, so count via the channel:
    // isPending(target) is true if any (type, subType, target) entry remains in
    // flight. Counts targets with at least one in-flight BRACKET_ENTRY —
    // sufficient for "are all peers caught up?".
    size_t count = 0;
    for (const auto& peer : bracket_) {
        if (bracketEntryChannel_->isPending(peer.data())) ++count;
    }
    return count;
}

int ShootoutManager::getCurrentMatchIndex() const {
    return currentMatchIndex_;
}

bool ShootoutManager::isLocalDuelist() const {
    const uint8_t* selfMac = wirelessManager_->getMacAddress();
    if (selfMac == nullptr) return false;
    return memcmp(selfMac, currentDuelistA_.data(), 6) == 0 ||
           memcmp(selfMac, currentDuelistB_.data(), 6) == 0;
}

std::array<uint8_t, 6> ShootoutManager::getOpponentMac() const {
    return opponentMac_;
}

uint8_t ShootoutManager::getLastMatchStartSeqId() const {
    return lastMatchStartSeqId_;
}

size_t ShootoutManager::getTournamentEndPendingAckCount() const {
    if (tournamentEndChannel_ == nullptr) return 0;
    size_t count = 0;
    for (const auto& peer : confirmedSet_) {
        if (tournamentEndChannel_->isPending(peer.data())) ++count;
    }
    return count;
}

std::array<uint8_t, 6> ShootoutManager::getTournamentWinner() const {
    return tournamentWinner_;
}

void ShootoutManager::setLoopMembersForTest(const std::vector<std::array<uint8_t, 6>>& members) {
    testLoopMembers_ = members;
    testLoopMembersOverride_ = !members.empty();

    // Claim coordinator on this device when self is the lowest-MAC member of
    // the simulated loop. Tests that need non-coordinator behavior pick a
    // self-mac that isn't the lowest, which leaves CDM coordinator clear.
    if (cdm_ != nullptr && wirelessManager_ != nullptr && !members.empty()) {
        const uint8_t* selfMac = wirelessManager_->getMacAddress();
        if (selfMac != nullptr) {
            const std::array<uint8_t, 6>* lowest = &members[0];
            for (const auto& m : members) {
                if (memcmp(m.data(), lowest->data(), 6) < 0) lowest = &m;
            }
            if (memcmp(lowest->data(), selfMac, 6) == 0) {
                cdm_->claimCoordinator();
            }
        }
    }
}

std::vector<std::array<uint8_t, 6>> ShootoutManager::getLoopMembers() const {
    if (testLoopMembersOverride_) return testLoopMembers_;
    return buildLoopMemberSet();
}

void ShootoutManager::resetToIdle() {
    LOG_W(TAG, "resetToIdle from phase=%d", static_cast<int>(phase_));
    phase_ = Phase::IDLE;
    // Drain any in-flight channel sends through the shared transport's
    // Resender. Pending entries are keyed by (PktType, subType, target);
    // iterate before clearing bracket_/confirmedSet_ below.
    auto cancelEachTarget = [this](auto* channel) {
        if (channel == nullptr) return;
        for (const auto& t : bracket_) channel->cancel(t.data());
        for (const auto& t : confirmedSet_) channel->cancel(t.data());
    };
    cancelEachTarget(bracketEntryChannel_);
    cancelEachTarget(matchStartChannel_);
    cancelEachTarget(matchResultChannel_);
    cancelEachTarget(tournamentEndChannel_);
    confirmedSet_.clear();
    bracket_.clear();
    currentRound_.clear();
    pendingBracket_.active = false;
    tournamentAbortPending_ = false;
    eliminated_.clear();
    reportedLocalWin_ = false;
    names_.clear();
    lastObservedBracketSeqId_ = 0;
    lastObservedMatchStartSeqId_ = 0;
    lastObservedTournamentEndSeqId_ = 0;
    currentMatchIndex_ = -1;
    memset(tournamentWinner_.data(), 0, 6);
    memset(opponentMac_.data(), 0, 6);
    memset(currentDuelistA_.data(), 0, 6);
    memset(currentDuelistB_.data(), 0, 6);
    memset(coordinatorMac_.data(), 0, 6);
}

void ShootoutManager::startProposal() {
    LOG_W(TAG, "startProposal");
    resetToIdle();
    phase_ = Phase::PROPOSAL;
}

void ShootoutManager::confirmLocal() {
    // Gate on PROPOSAL: stale ShootoutProposal button callbacks can fire in
    // later phases and re-advance the bracket if not guarded.
    if (phase_ != Phase::PROPOSAL) {
        LOG_W(TAG, "confirmLocal ignored; phase=%d", static_cast<int>(phase_));
        return;
    }
    LOG_W(TAG, "confirmLocal; confirmedCount before=%zu", confirmedSet_.size());
    const uint8_t* selfMac = wirelessManager_->getMacAddress();
    if (selfMac == nullptr) return;
    std::array<uint8_t, 6> mac;
    memcpy(mac.data(), selfMac, 6);
    if (!hasConfirmed(mac.data())) {
        confirmedSet_.push_back(mac);
    }
    if (player_ != nullptr) {
        recordName(selfMac, player_->getName().c_str());
    }
    sendLocalConfirm();
    if (allMembersConfirmed()) {
        LOG_W(TAG, "allMembersConfirmed -> advanceToBracketReveal");
        advanceToBracketReveal();
    }
}

void ShootoutManager::onConfirmReceived(const uint8_t* fromMac, const char* name) {
    // Non-coordinator entry: first inbound CONFIRM from a peer is how a
    // ring member learns the shootout is starting. Promote phase from IDLE
    // to PROPOSAL so subsequent gates accept the message and downstream
    // state transitions can fire (SupporterReady→Idle→ShootoutProposal).
    if (phase_ == Phase::IDLE && !isCoordinator()) {
        phase_ = Phase::PROPOSAL;
    }
    if (phase_ != Phase::PROPOSAL) return;
    const bool firstSeen = !hasConfirmed(fromMac);

    // Coordinator gates first-time CONFIRMs by loop-membership: it has the
    // authoritative member list (self + confirmedSupporters_ + OUTPUT_JACK
    // peer). Non-coordinators don't yet know who's in the loop (they'll
    // learn via BRACKET_ENTRY) and accept every CONFIRM until then.
    if (firstSeen && isCoordinator()) {
        auto members = getLoopMembers();
        bool inLoop = false;
        for (const auto& m : members) {
            if (memcmp(m.data(), fromMac, 6) == 0) { inLoop = true; break; }
        }
        if (!inLoop) return;
    }
    recordName(fromMac, name);
    if (firstSeen) {
        std::array<uint8_t, 6> mac;
        memcpy(mac.data(), fromMac, 6);
        confirmedSet_.push_back(mac);
        LOG_W(TAG, "onConfirmReceived from=%s count=%zu",
              MacToString(fromMac), confirmedSet_.size());

        // Ring forwarding: non-coordinators relay a first-time CONFIRM out
        // their OTHER direct-peer jack so the message walks around the ring
        // to the coordinator. The coordinator terminates propagation.
        if (!isCoordinator() && rdc_ != nullptr && confirmChannel_ != nullptr) {
            ShootoutConfirmPayload fwd{};
            fwd.cmd = static_cast<uint8_t>(ShootoutCmd::CONFIRM);
            fwd.seqId = 0;
            memcpy(fwd.mac, fromMac, 6);
            const std::string lookup = getNameForMac(fromMac);
            size_t copyLen = lookup.size() < kNameLength ? lookup.size() : kNameLength;
            memcpy(fwd.name, lookup.data(), copyLen);
            for (auto jack : {SerialIdentifier::INPUT_JACK, SerialIdentifier::OUTPUT_JACK}) {
                const uint8_t* direct = rdc_->getPeerMac(jack);
                if (direct == nullptr) continue;
                if (memcmp(direct, fromMac, 6) == 0) continue;  // don't echo to source
                confirmChannel_->sendOnce(direct, fwd);
            }
        }
    }

    if (allMembersConfirmed()) {
        LOG_W(TAG, "allMembersConfirmed -> advanceToBracketReveal");
        advanceToBracketReveal();
    }
}

void ShootoutManager::recordName(const uint8_t* mac, const char* name) {
    if (name == nullptr) return;
    char buf[kNameLength + 1];
    strncpy(buf, name, kNameLength);
    buf[kNameLength] = '\0';
    if (buf[0] == '\0') return;
    for (auto& entry : names_) {
        if (memcmp(entry.mac.data(), mac, 6) == 0) {
            entry.name = buf;
            return;
        }
    }
    NameEntry e;
    memcpy(e.mac.data(), mac, 6);
    e.name = buf;
    names_.push_back(std::move(e));
}

std::string ShootoutManager::getNameForMac(const uint8_t* mac) const {
    for (const auto& entry : names_) {
        if (memcmp(entry.mac.data(), mac, 6) == 0) return entry.name;
    }
    char fallback[4];
    snprintf(fallback, sizeof(fallback), "%02X", mac[5]);
    return fallback;
}

bool ShootoutManager::hasConfirmed(const uint8_t* mac) const {
    for (const auto& existing : confirmedSet_) {
        if (memcmp(existing.data(), mac, 6) == 0) return true;
    }
    return false;
}

bool ShootoutManager::allMembersConfirmed() const {
    auto members = getLoopMembers();
    if (members.empty()) return false;
    for (const auto& m : members) {
        if (!hasConfirmed(m.data())) return false;
    }
    return true;
}

std::array<uint8_t, 6> ShootoutManager::getCoordinatorMac() const {
    if (!bracket_.empty()) return coordinatorMac_;
    return lowestMacIn(confirmedSet_);
}

bool ShootoutManager::isCoordinator() const {
    // Pre-bracket: defer to CDM (the device that observed loop closure).
    // Post-bracket: anchor on coordinatorMac_, set when the bracket was
    // generated locally (or received from the originating coordinator via
    // BRACKET_ENTRY's fromMac). A mid-tournament CDM demote (heartbeat
    // timeout, transient cable nudge) must NOT change who runs the
    // tournament; the coordinator stays put until ABORT/END.
    const uint8_t* selfMac = wirelessManager_ ? wirelessManager_->getMacAddress() : nullptr;
    if (selfMac == nullptr) return false;
    bool coordSet = false;
    for (uint8_t b : coordinatorMac_) { if (b != 0) { coordSet = true; break; } }
    if (coordSet) return memcmp(coordinatorMac_.data(), selfMac, 6) == 0;
    return cdm_ != nullptr && cdm_->isCoordinator();
}

void ShootoutManager::generateBracket() {
    bracket_ = confirmedSet_;
    // Coordinator is whoever called generateBracket (i.e., self — we ran
    // through the coord-claim path and ended up here building the bracket).
    if (wirelessManager_ != nullptr && wirelessManager_->getMacAddress() != nullptr) {
        std::memcpy(coordinatorMac_.data(), wirelessManager_->getMacAddress(), 6);
    } else {
        coordinatorMac_ = lowestMacIn(bracket_);
    }
    // std::random_device is deterministic under newlib on ESP32, so seed
    // from platform clock XOR self-MAC to get real variation.
    unsigned long seed = 0;
    auto* clk = SimpleTimer::getPlatformClock();
    if (clk != nullptr) seed = clk->milliseconds();
    const uint8_t* selfMac = wirelessManager_->getMacAddress();
    if (selfMac != nullptr) {
        for (int i = 0; i < 6; i++) {
            seed ^= static_cast<unsigned long>(selfMac[i]) << ((i % 4) * 8);
        }
    }
    std::mt19937 rng(seed);
    std::shuffle(bracket_.begin(), bracket_.end(), rng);
    currentRound_ = bracket_;
}

void ShootoutManager::primeMatchManagerForMatch() {
    if (!matchManager_) return;
    if (!isLocalDuelist()) return;

    // Role-for-this-match from MAC ordering: both sides compute the same
    // ordering so the hunter_draw_time and bounty_time slots in MatchManager
    // are written by exactly one duelist each.
    const uint8_t* selfMac = wirelessManager_->getMacAddress();
    bool localIsHunterForMatch = selfMac != nullptr &&
        memcmp(selfMac, opponentMac_.data(), 6) < 0;

    char matchId[IdGenerator::UUID_BUFFER_SIZE];
    deriveShootoutMatchId(currentMatchIndex_, matchId, sizeof(matchId));
    LOG_W(TAG, "primeMatchManagerForMatch matchIndex=%d localHunter=%d",
          currentMatchIndex_, localIsHunterForMatch);
    // Pass the MAC-ordered slot to the match; do NOT touch global allegiance.
    matchManager_->initializeShootoutMatch(matchId, opponentMac_.data(), localIsHunterForMatch);
}

void ShootoutManager::advanceToBracketReveal() {
    phase_ = Phase::BRACKET_REVEAL;
    bracketRevealTimer_.setTimer(kBracketRevealMs);
    if (isCoordinator()) {
        generateBracket();
        sendBracketToPeers();
    }
}

void ShootoutManager::sendBracketToPeers() {
    if (bracket_.empty() || bracketEntryChannel_ == nullptr) return;
    if (bracket_.size() > MAX_SHOOTOUT_MEMBERS) {
        LOG_E(TAG, "bracket size %zu exceeds ESP-NOW peer cap (%u); aborting",
              bracket_.size(), (unsigned)MAX_SHOOTOUT_MEMBERS);
        abortTournament();
        return;
    }
    uint8_t batchId = nextBracketBatchId_++;
    if (nextBracketBatchId_ == 0) nextBracketBatchId_ = 1;
    uint8_t total = static_cast<uint8_t>(bracket_.size());
    const uint8_t* selfMac = wirelessManager_->getMacAddress();

    // lastBracketSeqId_ tracks the seqId of the FIRST slot sent, read by
    // onBracketAckReceived. Correct because sendReliable assigns seqIds
    // monotonically and the first call wins the cache.
    bool firstSent = false;
    for (const auto& target : bracket_) {
        if (selfMac && std::memcmp(target.data(), selfMac, 6) == 0) continue;
        for (uint8_t i = 0; i < total; ++i) {
            ShootoutBracketEntryPayload p{};
            p.cmd = static_cast<uint8_t>(ShootoutCmd::BRACKET_ENTRY);
            p.batchId = batchId;
            p.slot = i;
            p.totalSlots = total;
            std::memcpy(p.mac, bracket_[i].data(), 6);
            uint8_t seq = bracketEntryChannel_->sendReliable(target.data(), p);
            if (!firstSent) {
                lastBracketSeqId_ = seq;
                firstSent = true;
            }
        }
    }
}

void ShootoutManager::onBracketAckReceived(const uint8_t* fromMac, uint8_t seqId) {
    // Channel-based BRACKET_ENTRY sends auto-ack via transport->onAckPacket on
    // the receiving side, but unit tests call this method directly without
    // driving the wireless path. Translate the legacy ack into a cancel of
    // every pending BRACKET_ENTRY entry to `fromMac` so the test fixture sees
    // pendingCount drop to zero.
    (void)seqId;
    if (bracketEntryChannel_ != nullptr) {
        bracketEntryChannel_->cancel(fromMac);
    }
}

void ShootoutManager::abortTournament() {
    // ENDED is terminal-success; this guard stops a straggling
    // MATCH_RESULT abandon from latching tournamentAbortPending_
    // and demoting the success state.
    if (phase_ == Phase::ABORTED || phase_ == Phase::ENDED) return;
    LOG_W(TAG, "abortTournament from phase=%d", static_cast<int>(phase_));

    // Broadcast before resetToIdle clears bracket_/confirmedSet_.
    if (abortChannel_ != nullptr) {
        ShootoutAbortPayload p{};
        p.cmd = static_cast<uint8_t>(ShootoutCmd::ABORT);
        p.seqId = 0;
        const auto& targets = bracket_.empty() ? confirmedSet_ : bracket_;
        const uint8_t* selfMac = wirelessManager_->getMacAddress();
        for (const auto& m : targets) {
            if (selfMac != nullptr && memcmp(m.data(), selfMac, 6) == 0) continue;
            abortChannel_->sendOnce(m.data(), p);
        }
    }

    resetToIdle();
    phase_ = Phase::ABORTED;
}

void ShootoutManager::sendLocalConfirm() {
    const uint8_t* selfMac = wirelessManager_->getMacAddress();
    if (selfMac == nullptr) return;

    ShootoutConfirmPayload p{};
    p.cmd = static_cast<uint8_t>(ShootoutCmd::CONFIRM);
    p.seqId = 0;
    memcpy(p.mac, selfMac, 6);
    memset(p.name, 0, kNameLength);
    if (player_ != nullptr) {
        const std::string& n = player_->getName();
        size_t copyLen = n.size() < kNameLength ? n.size() : kNameLength;
        memcpy(p.name, n.data(), copyLen);
    }

    if (confirmChannel_ == nullptr) return;

    // Coordinator addresses every loop member directly (it has the full set
    // in getLoopMembers()). Non-coordinators don't yet know the membership;
    // they send to both RDC direct peers and rely on each receiver
    // forwarding to its other direct peer to propagate around the ring.
    if (isCoordinator()) {
        for (const auto& m : getLoopMembers()) {
            if (memcmp(m.data(), selfMac, 6) == 0) continue;
            confirmChannel_->sendOnce(m.data(), p);
        }
    } else if (rdc_ != nullptr) {
        for (auto jack : {SerialIdentifier::INPUT_JACK, SerialIdentifier::OUTPUT_JACK}) {
            const uint8_t* direct = rdc_->getPeerMac(jack);
            if (direct != nullptr) confirmChannel_->sendOnce(direct, p);
        }
    }
    confirmRebroadcastTimer_.setTimer(kConfirmRebroadcastMs);
}

void ShootoutManager::sync() {
    // Check cheap conditions before allMembersConfirmed(), which rebuilds the
    // loop-member set each call.
    if (phase_ == Phase::PROPOSAL && confirmRebroadcastTimer_.expired()) {
        const uint8_t* selfMac = wirelessManager_->getMacAddress();
        if (selfMac != nullptr && hasConfirmed(selfMac) && !allMembersConfirmed()) {
            sendLocalConfirm();
        }
    }

    if (transport_) transport_->sync();
    if (tournamentAbortPending_) {
        tournamentAbortPending_ = false;
        abortTournament();
    }

    maybeStartNextMatch();
}

std::pair<std::array<uint8_t,6>, std::array<uint8_t,6>>
ShootoutManager::getCurrentMatchPair() const {
    if (currentMatchIndex_ < 0) return {};
    return {currentDuelistA_, currentDuelistB_};
}

void ShootoutManager::sendMatchStartToPeers(int matchIndex) {
    const uint8_t* selfMac = wirelessManager_->getMacAddress();
    const auto& a = currentRound_[matchIndex * 2];
    const auto& b = currentRound_[matchIndex * 2 + 1];
    bool sameMatch = isSameMatch(matchIndex, a.data(), b.data());
    currentDuelistA_ = a;
    currentDuelistB_ = b;
    currentMatchIndex_ = matchIndex;
    if (!sameMatch) reportedLocalWin_ = false;
    if (!sameMatch && isLocalDuelist() && selfMac != nullptr) {
        const uint8_t* opp = (memcmp(selfMac, a.data(), 6) == 0) ? b.data() : a.data();
        memcpy(opponentMac_.data(), opp, 6);
        primeMatchManagerForMatch();
    }

    if (matchStartChannel_ == nullptr) return;
    ShootoutMatchStartPayload p{};
    p.cmd = static_cast<uint8_t>(ShootoutCmd::MATCH_START);
    std::memcpy(p.duelistA, a.data(), 6);
    std::memcpy(p.duelistB, b.data(), 6);
    p.matchIndex = static_cast<uint8_t>(matchIndex);

    bool firstSent = false;
    for (const auto& target : bracket_) {
        if (selfMac != nullptr && std::memcmp(target.data(), selfMac, 6) == 0) continue;
        uint8_t seq = matchStartChannel_->sendReliable(target.data(), p);
        if (!firstSent) {
            lastMatchStartSeqId_ = seq;
            firstSent = true;
        }
    }
}

void ShootoutManager::onMatchStartAckReceived(const uint8_t* fromMac, uint8_t seqId) {
    (void)seqId;
    if (matchStartChannel_ != nullptr) matchStartChannel_->cancel(fromMac);
}

bool ShootoutManager::isSameMatch(int matchIndex, const uint8_t* a, const uint8_t* b) const {
    return matchIndex == currentMatchIndex_
        && phase_ == Phase::MATCH_IN_PROGRESS
        && memcmp(currentDuelistA_.data(), a, 6) == 0
        && memcmp(currentDuelistB_.data(), b, 6) == 0;
}

bool ShootoutManager::isActiveDuelist(const uint8_t* mac) const {
    if (currentMatchIndex_ < 0) return false;
    auto pair = getCurrentMatchPair();
    return memcmp(pair.first.data(), mac, 6) == 0 ||
           memcmp(pair.second.data(), mac, 6) == 0;
}


void ShootoutManager::onDirectPeerChange(SerialIdentifier /*port*/,
                                         std::optional<RemoteDeviceCoordinator::Peer> previous,
                                         std::optional<RemoteDeviceCoordinator::Peer> current) {
    // Connect transitions don't change tournament state; the peer must have
    // already participated in CONFIRM/BRACKET to be a duelist. Disconnect
    // routes into onLocalRDCDisconnect, which decides whether to broadcast
    // PEER_LOST.
    if (current.has_value() || !previous.has_value()) return;
    onLocalRDCDisconnect(previous->mac.data());
}

void ShootoutManager::onLocalRDCDisconnect(const uint8_t* lostMac) {
    LOG_W(TAG, "onLocalRDCDisconnect %s phase=%d",
          MacToString(lostMac), static_cast<int>(phase_));
    if (phase_ == Phase::IDLE || phase_ == Phase::ABORTED || phase_ == Phase::ENDED) return;
    // Stale-disconnect filter: if the "lost" peer is still our RDC direct
    // peer (handshake hasn't actually torn down), the loss is informational
    // and PEER_LOST shouldn't fire. Transitive reachability isn't tracked here;
    // bracket-side filtering happens via the coordinator's ABORT-on-demotion path.
    if (rdc_ && rdc_->isDirectPeer(lostMac)) return;
    if (peerLostChannel_ != nullptr) {
        ShootoutPeerLostPayload p{};
        p.cmd = static_cast<uint8_t>(ShootoutCmd::PEER_LOST);
        p.seqId = 0;
        memcpy(p.mac, lostMac, 6);
        const auto& targets = bracket_.empty() ? confirmedSet_ : bracket_;
        const uint8_t* selfMac = wirelessManager_->getMacAddress();
        for (const auto& m : targets) {
            if (selfMac != nullptr && memcmp(m.data(), selfMac, 6) == 0) continue;
            peerLostChannel_->sendOnce(m.data(), p);
        }
    }
    onPeerLostReceived(lostMac);
}

void ShootoutManager::onPeerLostReceived(const uint8_t* lostMac) {
    LOG_W(TAG, "onPeerLostReceived %s phase=%d",
          MacToString(lostMac), static_cast<int>(phase_));
    if (phase_ == Phase::IDLE || phase_ == Phase::ABORTED || phase_ == Phase::ENDED) return;
    // If the "lost" peer is still our direct RDC peer the message is stale
    // for us (we'd observe the loss locally first). Otherwise the tournament
    // participant is gone and we abort.
    if (rdc_ && rdc_->isDirectPeer(lostMac)) return;
    abortTournament();
}

void ShootoutManager::maybeStartNextMatch() {
    if (!isCoordinator()) return;
    if (getBracketPendingAckCount() != 0) return;
    if (phase_ != Phase::BRACKET_REVEAL && phase_ != Phase::BETWEEN_MATCHES) return;
    if (phase_ == Phase::BRACKET_REVEAL && !bracketRevealTimer_.expired()) return;
    // Re-entrancy guard: reportLocalWin calls maybeStartNextMatch directly,
    // and the same sync() tick can then re-enter through the match-advance
    // path. Both run on the main loop (ESP-NOW packets land in the drain
    // queue and dispatch from sync()), so this guards same-tick recursion,
    // not cross-thread concurrency.
    if (inMaybeStartNextMatch_) return;
    inMaybeStartNextMatch_ = true;
    struct Guard { bool& f; ~Guard() { f = false; } } guard{inMaybeStartNextMatch_};

    currentMatchIndex_++;
    int pairEnd = currentMatchIndex_ * 2 + 1;
    if (pairEnd >= (int)currentRound_.size()) {
        std::vector<std::array<uint8_t, 6>> survivors;
        for (const auto& m : currentRound_) {
            if (!isEliminated(m.data())) survivors.push_back(m);
        }
        if (survivors.size() <= 1) {
            auto winner = survivors.empty()
                ? findLastRemaining()
                : survivors[0];
            sendTournamentEndToPeers(winner.data());
            return;
        }
        LOG_W(TAG, "advancing round: %zu survivors -> %zu",
              currentRound_.size(), survivors.size());
        currentRound_ = survivors;
        currentMatchIndex_ = 0;
    }
    sendMatchStartToPeers(currentMatchIndex_);
    phase_ = Phase::MATCH_IN_PROGRESS;
}

std::array<uint8_t, 6> ShootoutManager::lowestMacIn(
    const std::vector<std::array<uint8_t, 6>>& set) {
    std::array<uint8_t, 6> lowest = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    for (const auto& m : set) {
        if (memcmp(m.data(), lowest.data(), 6) < 0) lowest = m;
    }
    return lowest;
}

void ShootoutManager::onBracketReceived(
    const std::vector<std::array<uint8_t, 6>>& bracket, uint8_t seqId) {
    // Test-only entry point: production receive path goes through the
    // BRACKET_ENTRY channel installed in initialize(). Acks are emitted by
    // ReliableChannel::deliver on the wire path; this synthesized path skips
    // them since callers are bypassing the channel for state injection.
    if (isCoordinator()) return;
    if (seqId != 0 && seqId == lastObservedBracketSeqId_) return;
    lastObservedBracketSeqId_ = seqId;
    bracket_ = bracket;
    currentRound_ = bracket;
    coordinatorMac_ = lowestMacIn(bracket_);
    phase_ = Phase::BRACKET_REVEAL;
    bracketRevealTimer_.setTimer(kBracketRevealMs);
}

void ShootoutManager::onMatchStartReceived(
    const uint8_t* duelistA, const uint8_t* duelistB,
    uint8_t matchIndex, uint8_t seqId) {
    // Test-only entry point: production receive routes through matchStartChannel_.
    if (isCoordinator()) return;
    if (seqId != 0 && seqId == lastObservedMatchStartSeqId_) return;
    bool sameMatch = isSameMatch(matchIndex, duelistA, duelistB);
    lastObservedMatchStartSeqId_ = seqId;
    if (sameMatch) return;
    currentMatchIndex_ = matchIndex;
    memcpy(currentDuelistA_.data(), duelistA, 6);
    memcpy(currentDuelistB_.data(), duelistB, 6);
    phase_ = Phase::MATCH_IN_PROGRESS;
    reportedLocalWin_ = false;
    const uint8_t* selfMac = wirelessManager_->getMacAddress();
    if (isLocalDuelist() && selfMac != nullptr) {
        const uint8_t* opp = (memcmp(selfMac, duelistA, 6) == 0) ? duelistB : duelistA;
        memcpy(opponentMac_.data(), opp, 6);
        primeMatchManagerForMatch();
    }
}

bool ShootoutManager::isEliminated(const uint8_t* mac) const {
    for (const auto& m : eliminated_) {
        if (memcmp(m.data(), mac, 6) == 0) return true;
    }
    return false;
}

void ShootoutManager::applyMatchResult(const uint8_t* winner, const uint8_t* loser) {
    if (!isEliminated(loser)) {
        std::array<uint8_t, 6> mac;
        memcpy(mac.data(), loser, 6);
        eliminated_.push_back(mac);
    }
    phase_ = Phase::BETWEEN_MATCHES;
}

void ShootoutManager::sendMatchResultToPeers(
    const uint8_t* winner, const uint8_t* loser, uint8_t matchIndex) {
    if (matchResultChannel_ == nullptr) return;
    ShootoutMatchResultPayload p{};
    p.cmd = static_cast<uint8_t>(ShootoutCmd::MATCH_RESULT);
    std::memcpy(p.winner, winner, 6);
    std::memcpy(p.loser, loser, 6);
    p.matchIndex = matchIndex;

    const uint8_t* selfMac = wirelessManager_->getMacAddress();
    bool firstSent = false;
    for (const auto& target : confirmedSet_) {
        if (selfMac != nullptr && std::memcmp(target.data(), selfMac, 6) == 0) continue;
        uint8_t seq = matchResultChannel_->sendReliable(target.data(), p);
        if (!firstSent) {
            lastMatchResultSeqId_ = seq;
            firstSent = true;
        }
    }
}

void ShootoutManager::reportLocalWin() {
    const uint8_t* selfMac = wirelessManager_->getMacAddress();
    if (selfMac == nullptr) return;
    if (reportedLocalWin_) return;
    reportedLocalWin_ = true;
    LOG_W(TAG, "reportLocalWin matchIndex=%d", currentMatchIndex_);
    sendMatchResultToPeers(selfMac, opponentMac_.data(), static_cast<uint8_t>(currentMatchIndex_));
    applyMatchResult(selfMac, opponentMac_.data());
    if (isCoordinator()) maybeStartNextMatch();
}

void ShootoutManager::onMatchResultReceived(
    const uint8_t* winner, const uint8_t* loser,
    uint8_t matchIndex, uint8_t seqId, const uint8_t* fromMac) {
    // Production path acks via ReliableChannel::deliver before this fires.
    // Tests inject MATCH_RESULT directly without driving the channel, so they
    // already get the response semantics they need from this call.
    (void)seqId; (void)fromMac;
    // Dedup by loser-MAC rather than seqId: non-coord senders have independent
    // seq counters, but each loser is eliminated exactly once per tournament.
    if (isEliminated(loser)) {
        return;
    }
    LOG_W(TAG, "onMatchResultReceived matchIndex=%u", matchIndex);
    applyMatchResult(winner, loser);
    if (isCoordinator()) maybeStartNextMatch();
}

void ShootoutManager::onMatchResultAckReceived(const uint8_t* fromMac, uint8_t seqId) {
    (void)seqId;
    if (matchResultChannel_ != nullptr) matchResultChannel_->cancel(fromMac);
}

size_t ShootoutManager::getMatchResultPendingAckCount() const {
    if (matchResultChannel_ == nullptr) return 0;
    size_t count = 0;
    for (const auto& peer : confirmedSet_) {
        if (matchResultChannel_->isPending(peer.data())) ++count;
    }
    return count;
}

std::array<uint8_t, 6> ShootoutManager::findLastRemaining() const {
    for (const auto& m : bracket_) {
        if (!isEliminated(m.data())) return m;
    }
    return {};
}

void ShootoutManager::sendTournamentEndToPeers(const uint8_t* winner) {
    LOG_W(TAG, "tournamentEnd winner=%s", MacToString(winner));
    if (tournamentEndChannel_ != nullptr) {
        ShootoutTournamentEndPayload p{};
        p.cmd = static_cast<uint8_t>(ShootoutCmd::TOURNAMENT_END);
        std::memcpy(p.winner, winner, 6);
        const uint8_t* selfMac = wirelessManager_->getMacAddress();
        bool firstSent = false;
        // Targets confirmedSet_ rather than bracket_: eliminated players need the
        // tournament-end transition or they stall in BETWEEN_MATCHES.
        for (const auto& target : confirmedSet_) {
            if (selfMac != nullptr && std::memcmp(target.data(), selfMac, 6) == 0) continue;
            uint8_t seq = tournamentEndChannel_->sendReliable(target.data(), p);
            if (!firstSent) {
                lastTournamentEndSeqId_ = seq;
                firstSent = true;
            }
        }
    }
    memcpy(tournamentWinner_.data(), winner, 6);
    phase_ = Phase::ENDED;
    // Drop any in-flight MATCH_RESULT pending so a straggling abandon does
    // not latch tournamentAbortPending_ after the tournament has ended.
    if (matchResultChannel_ != nullptr) {
        // The channel's pending entries live in the shared transport Resender.
        // Cancel each peer that still has a MATCH_RESULT in flight.
        for (const auto& target : confirmedSet_) {
            matchResultChannel_->cancel(target.data());
        }
    }
}

void ShootoutManager::onTournamentEndAckReceived(const uint8_t* fromMac, uint8_t seqId) {
    (void)seqId;
    if (tournamentEndChannel_ != nullptr) tournamentEndChannel_->cancel(fromMac);
}

void ShootoutManager::onTournamentEndReceived(const uint8_t* winner, uint8_t seqId) {
    if (seqId != 0 && seqId == lastObservedTournamentEndSeqId_) return;
    lastObservedTournamentEndSeqId_ = seqId;
    memcpy(tournamentWinner_.data(), winner, 6);
    phase_ = Phase::ENDED;
    // Drop any in-flight MATCH_RESULT pending so a straggling abandon does
    // not latch tournamentAbortPending_ after the tournament has ended.
    if (matchResultChannel_ != nullptr) {
        for (const auto& target : confirmedSet_) {
            matchResultChannel_->cancel(target.data());
        }
    }
}

void ShootoutManager::onAbortReceived() {
    // ENDED is terminal-success and must not be demoted by a stray ABORT
    // from a peer that diverged.
    if (phase_ == Phase::ABORTED || phase_ == Phase::IDLE || phase_ == Phase::ENDED) return;
    resetToIdle();
    phase_ = Phase::ABORTED;
}

std::vector<std::array<uint8_t, 6>> ShootoutManager::buildLoopMemberSet() const {
    // Building a bracket from mid-convergence partial topology state produces
    // inconsistent members across devices. Wait for the peer-graph topology to
    // settle (isTopologyStable) before opening any derived state from it.
    if (rdc_ == nullptr || !rdc_->isTopologyStable()) return {};
    return rdc_->getChainMembers();
}
