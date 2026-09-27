#include "game/shootout-manager.hpp"
#include "game/match-manager.hpp"
#include "device/drivers/logger.hpp"
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
                                 RemoteDeviceCoordinator* rdc)
    : player(player)
    , wirelessManager(wirelessManager)
    , rdc(rdc)
    , resender(wirelessManager, Resender::BudgetPolicy::EVERY_ROUND) {
    resender.setAbandonCallback(
        [this](PktType, uint8_t seqId, const uint8_t* targetMac,
               const uint8_t* packet, size_t len) {
            onCommandAbandoned(seqId, targetMac, packet, len);
        });
    if (rdc == nullptr) return;
    // Subscribed here rather than by whoever builds this manager: see
    // ChainDuelManager's constructor for the reasoning.
    rdc->setOnRingClosed([this]() { onRingClosed(); });
}

ShootoutManager::~ShootoutManager() {
    if (rdc == nullptr) return;
    rdc->setOnRingClosed(nullptr);
}

bool ShootoutManager::active() const {
    return phase != Phase::IDLE;
}

ShootoutManager::Phase ShootoutManager::getPhase() const {
    return phase;
}

size_t ShootoutManager::getConfirmedCount() const {
    return confirmedSet.size();
}

std::vector<std::array<uint8_t, 6>> ShootoutManager::getBracket() const {
    return bracket;
}

size_t ShootoutManager::getPendingAckCount(uint8_t seqId) const {
    return resender.pendingCount(PktType::kShootoutCommand, seqId);
}

uint8_t ShootoutManager::getLastBracketSeqId() const {
    return lastBracketSeqId;
}

int ShootoutManager::getCurrentMatchIndex() const {
    return currentMatchIndex;
}

bool ShootoutManager::isLocalDuelist() const {
    const uint8_t* selfMac = wirelessManager->getMacAddress();
    if (selfMac == nullptr) return false;
    return memcmp(selfMac, currentDuelistA.data(), 6) == 0 ||
           memcmp(selfMac, currentDuelistB.data(), 6) == 0;
}

// The single seqId allocator for every reliably-sent command this manager
// sends — the five that ride the Resender; the rest go out unsequenced. That
// is load-bearing, not incidental: because one counter serves all of them, a
// seqId in flight names exactly one of this device's frames, which is what lets
// an ack be answered on its seqId alone.
uint8_t ShootoutManager::nextSeqId() {
    uint8_t id = nextShootoutSeqId++;
    if (nextShootoutSeqId == 0) nextShootoutSeqId = 1;
    return id;
}

void ShootoutManager::addMac(std::vector<std::array<uint8_t, 6>>& set, const uint8_t* mac) {
    if (containsMac(set, mac)) return;
    std::array<uint8_t, 6> entry;
    memcpy(entry.data(), mac, 6);
    set.push_back(entry);
}

bool ShootoutManager::containsMac(const std::vector<std::array<uint8_t, 6>>& set,
                                  const uint8_t* mac) {
    if (mac == nullptr) return false;
    for (const std::array<uint8_t, 6>& entry : set) {
        if (memcmp(entry.data(), mac, 6) == 0) return true;
    }
    return false;
}

bool ShootoutManager::isFromCoordinator(const uint8_t* mac) const {
    // An all-zero anchor (no bracket held yet) matches no real sender.
    return mac != nullptr && memcmp(mac, coordinatorMac.data(), 6) == 0;
}

bool ShootoutManager::isRingMember(const uint8_t* mac) const {
    if (containsMac(bracket, mac)) return true;
    if (containsMac(confirmedSet, mac)) return true;
    return containsMac(getLoopMembers(), mac);
}

void ShootoutManager::broadcastCommand(const uint8_t* packet, size_t len) {
    wirelessManager->sendEspNowData(wirelessManager->getBroadcastAddress(),
                                    PktType::kShootoutCommand, packet, len);
}

void ShootoutManager::broadcastToRing(const std::vector<std::array<uint8_t, 6>>& peers,
                                      const uint8_t* packet, size_t len) {
    // A ring fan-out is one broadcast frame, not one unicast per member: the
    // ESP-NOW peer table holds 20 entries, so unicast addressing cannot reach a
    // ring larger than that at all, whereas the broadcast slot is registered once
    // at radio init. Receivers must drop commands naming MACs outside their own ring.
    if (peersExcludingSelf(peers).empty()) return;
    broadcastCommand(packet, len);
}

// Who a ring fan-out is addressed to. One spelling, asked by both send paths, so
// a device can never end up owing an ack to itself.
std::vector<std::array<uint8_t, 6>> ShootoutManager::peersExcludingSelf(
    const std::vector<std::array<uint8_t, 6>>& peers) const {
    const uint8_t* selfMac = wirelessManager->getMacAddress();
    std::vector<std::array<uint8_t, 6>> others;
    for (const std::array<uint8_t, 6>& m : peers) {
        if (selfMac != nullptr && memcmp(m.data(), selfMac, 6) == 0) continue;
        others.push_back(m);
    }
    return others;
}

void ShootoutManager::sendReliablyToPeers(const std::vector<std::array<uint8_t, 6>>& peers,
                                          uint8_t seqId, const uint8_t* packet, size_t len) {
    resender.sendBroadcast(peersExcludingSelf(peers), PktType::kShootoutCommand,
                           seqId, packet, len);
}

void ShootoutManager::onCommandAckReceived(const uint8_t* fromMac, uint8_t seqId) {
    // seqId alone names the fan-out: nextSeqId() is the single allocator for all
    // five command families, so no two frames in flight from this device share
    // one. Cross-checking the ack's command against a per-family cursor would
    // catch nothing — an ack echoes both fields out of the frame it answers —
    // and would refuse a valid ack for a still-armed frame that is no longer its
    // family's latest.
    resender.onAck(PktType::kShootoutCommand, seqId, fromMac);
}

void ShootoutManager::onCommandAbandoned(uint8_t seqId, const uint8_t* targetMac,
                                         const uint8_t* packet, size_t len) {
    if (len == 0) return;
    const ShootoutCmd cmd = static_cast<ShootoutCmd>(packet[0]);

    // Only silence that actually blocks the tournament ends it. BRACKET is the
    // roster, so a member still in the running who never received it cannot take
    // part at all. MATCH_START only matters to the two devices fighting: a
    // spectator missing it just does not see that round, and ending a tournament
    // for that would put an abort opportunity on every member of every match
    // rather than one on the bracket.
    //
    // Both questions are asked of the frame that was abandoned, not of the
    // manager's current state. A fan-out outlives the match it announced — it
    // keeps retrying a silent recipient for over a second — so by the time it is
    // given up on, currentDuelist* may already name a different match, and a
    // finished tournament still names its final pair.
    bool blocksTournament = false;
    if (cmd == ShootoutCmd::BRACKET) {
        blocksTournament = containsMac(bracket, targetMac) && !isEliminated(targetMac);
    } else if (cmd == ShootoutCmd::MATCH_START && len >= 14) {
        // [cmd, seqId, duelistA(6), duelistB(6), matchIndex] — see
        // buildMatchStartPacket.
        blocksTournament = memcmp(&packet[2], targetMac, 6) == 0 ||
                           memcmp(&packet[8], targetMac, 6) == 0;
    } else if (cmd == ShootoutCmd::MATCH_RESULT && len >= 15 &&
               memcmp(targetMac, coordinatorMac.data(), 6) == 0) {
        // Nothing on the coordinator's side notices a result that never lands: it
        // advances only on receiving one, so it waits with nothing owed. This
        // device is the one that knows, so it says so again. One attempt per
        // bout — a second abandonment cannot distinguish a lost result from a
        // lost ack, and guessing is worse than staying quiet.
        if (matchResultResentIndex != static_cast<int>(packet[14])) {
            matchResultResentIndex = static_cast<int>(packet[14]);
            LOG_W(TAG, "coordinator missed our match result; re-sending");
            // Off the abandoned frame, not current state: a fan-out outlives the
            // match it announced. [cmd, seqId, winner(6), loser(6), matchIndex]
            sendMatchResultToPeers(&packet[2], &packet[8], packet[14]);
        }
    }

    if (blocksTournament) {
        LOG_E(TAG, "shootout cmd=%u seq=%u unacked by %s; ending tournament",
              (unsigned)packet[0], (unsigned)seqId, MacToString(targetMac));
        abortTournament();
        return;
    }
    LOG_W(TAG, "shootout cmd=%u seq=%u unacked by %s",
          (unsigned)packet[0], (unsigned)seqId, MacToString(targetMac));
}

std::array<uint8_t, 6> ShootoutManager::getOpponentMac() const {
    return opponentMac;
}

uint8_t ShootoutManager::getLastMatchStartSeqId() const {
    return lastMatchStartSeqId;
}

std::array<uint8_t, 6> ShootoutManager::getTournamentWinner() const {
    return tournamentWinner;
}

void ShootoutManager::setLoopMembersForTest(const std::vector<std::array<uint8_t, 6>>& members) {
    testLoopMembers = members;
    testLoopMembersOverride = !members.empty();
}

std::vector<std::array<uint8_t, 6>> ShootoutManager::getLoopMembers() const {
    if (testLoopMembersOverride) return testLoopMembers;
    return buildLoopMemberSet();
}

void ShootoutManager::resetToIdle() {
    LOG_W(TAG, "resetToIdle from phase=%d", static_cast<int>(phase));
    resetTournamentState();
    ringMembers.clear();
    ringClosedRebroadcastTimer.invalidate();
}

void ShootoutManager::resetTournamentState() {
    phase = Phase::IDLE;
    // Retire a bout this tournament primed: the duel app's dismount is the only
    // other path and cannot run for a bout abandoned before that app mounts,
    // and Idle mounts a duel from whatever match is ready.
    if (matchManager) matchManager->clearShootoutMatch();
    confirmedSet.clear();
    bracket.clear();
    currentRound.clear();
    memset(coordinatorMac.data(), 0, 6);
    // A retransmit landing after the reset would speak for a tournament that no
    // longer exists — except the frame reporting the ending, whose recipients are
    // exactly the members yet to hear it. Both the screen showing the ending and
    // the proposal after it reset, so it has to survive more than one.
    resender.cancelAllExcept(PktType::kShootoutCommand, terminalFanOutSeqId);
    if (getPendingAckCount(terminalFanOutSeqId) == 0) terminalFanOutSeqId = 0;
    eliminated.clear();
    // Sampled only while the phase is PROPOSAL, so a tournament that leaves that
    // phase mid-window would carry a part-aged timer into the next one.
    shortRosterDebounce.reset();
    proposalTimer.invalidate();
    endingRebroadcastTimer.invalidate();
    reportedLocalWin = false;
    names.clear();
    currentMatchIndex = -1;
    memset(tournamentWinner.data(), 0, 6);
    memset(opponentMac.data(), 0, 6);
    memset(currentDuelistA.data(), 0, 6);
    memset(currentDuelistB.data(), 0, 6);
}

void ShootoutManager::startProposal() {
    LOG_W(TAG, "startProposal");
    resetTournamentState();
    if (headsRing()) {
        // A new tournament is a new roster: re-read ring detection rather than
        // carrying what the last one accumulated, which could name a device
        // unplugged since and stall the proposal above the short-roster floor.
        // Announcing it is sync()'s: on the first tick after an abort, which clears
        // the rebroadcast timer, and within the second otherwise.
        ringMembers = getLoopMembers();
        LOG_W(TAG, "proposing to members=%zu", ringMembers.size());
    }
    proposalTimer.setTimer(PROPOSAL_TIMEOUT_MS);
    phase = Phase::PROPOSAL;
}

void ShootoutManager::onRingClosed() {
    if (phase != Phase::IDLE) {
        LOG_W(TAG, "onRingClosed ignored; phase=%d", static_cast<int>(phase));
        return;
    }
    const uint8_t* selfMac = wirelessManager->getMacAddress();
    if (selfMac == nullptr) {
        LOG_E(TAG, "onRingClosed with no local MAC");
        return;
    }
    // The RDC fires this on the device whose own MAC came back around the ring.
    // It only announces the roster: who coordinates is read from ring detection
    // when a bracket is drawn, because a second latch this same edge fired on can
    // resolve away before then.
    ringMembers = getLoopMembers();
    LOG_W(TAG, "ring closed; announcing members=%zu", ringMembers.size());
    sendRingClosed();
}

void ShootoutManager::onRingClosedReceived(
    const uint8_t* fromMac, const std::vector<std::array<uint8_t, 6>>& members) {
    const uint8_t* selfMac = wirelessManager->getMacAddress();
    if (selfMac == nullptr) {
        LOG_E(TAG, "onRingClosedReceived with no local MAC");
        return;
    }
    // A broadcast reaches every ring in radio range; the roster is what says
    // whether this closure is ours.
    if (!containsMac(members, selfMac)) return;
    const uint8_t* head = ringHead();
    const bool fromOurHead = head != nullptr && memcmp(fromMac, head, 6) == 0;
    // The device running our tournament is announcing a ring, so it is running no
    // tournament: neither sender of RING_CLOSED holds a bracket — onRingClosed
    // refuses anything but IDLE, and the repeat lives inside the proposal block,
    // which runs with the bracket already cleared. Keyed on the coordinator rather
    // than the current head, because those are deliberately not the same device: a
    // head that moves mid-tournament hands nothing over, so a new head announcing a
    // ring says nothing about the tournament this device is still in. A finished one
    // stays finished: ENDED keeps the bracket standing on purpose, and retiring it
    // would zero the winner on screen. Giving up is local — an ABORT fan-out would
    // reach that fresh proposal, which takes one from any ring member.
    if (isFromCoordinator(fromMac) && !bracket.empty() && !isTerminalPhase()) {
        LOG_W(TAG, "our head announced a ring; retiring the bracket we hold");
        resetToIdle();
        phase = Phase::ABORTED;
        return;
    }
    // A member's roster is its head's, so the head's rebroadcast replaces one this
    // device recorded from a latch of its own that ring detection resolved away.
    if (phase != Phase::IDLE && !(phase == Phase::PROPOSAL && fromOurHead)) return;
    ringMembers = members;
    LOG_W(TAG, "ring closed by %s members=%zu", MacToString(fromMac), members.size());
}

bool ShootoutManager::shouldEnterProposal() const {
    if (phase != Phase::IDLE || rdc == nullptr) return false;
    // The head proposes off its latch, which outlives the spent ring-closed edge.
    // A member needs a roster to gate confirms against and a head to take the
    // bracket from; the head is read live, so a RING_CLOSED kept from a ring that
    // has since broken cannot start a tournament on its own.
    if (headsRing()) return true;
    return !ringMembers.empty() && ringHead() != nullptr;
}

void ShootoutManager::sendRingClosed() {
    // seqId 0: receipt is idempotent and deduped by phase, so no ack round-trip.
    std::vector<uint8_t> packet = buildMacListPacket(ShootoutCmd::RING_CLOSED, 0, ringMembers);
    broadcastToRing(ringMembers, packet.data(), packet.size());
    ringClosedRebroadcastTimer.setTimer(kConfirmRebroadcastMs);
}

void ShootoutManager::confirmLocal() {
    // Gate on PROPOSAL: stale ShootoutProposal button callbacks can fire in
    // later phases and re-advance the bracket if not guarded.
    if (phase != Phase::PROPOSAL) {
        LOG_W(TAG, "confirmLocal ignored; phase=%d", static_cast<int>(phase));
        return;
    }
    LOG_W(TAG, "confirmLocal; confirmedCount before=%zu", confirmedSet.size());
    const uint8_t* selfMac = wirelessManager->getMacAddress();
    if (selfMac == nullptr) return;
    addMac(confirmedSet, selfMac);
    if (player != nullptr) {
        recordName(selfMac, player->getName().c_str());
    }
    sendLocalConfirm();
}

void ShootoutManager::onConfirmReceived(const uint8_t* fromMac, const char* name) {
    if (phase != Phase::PROPOSAL) return;
    // Fast path: already-confirmed peers bypass the loop-membership scan (this
    // is the common case during 1Hz rebroadcasts — the gate only needs to
    // block first-time stray CONFIRMs from outside the ring).
    if (!hasConfirmed(fromMac) && !containsMac(getLoopMembers(), fromMac)) return;
    recordName(fromMac, name);
    if (!hasConfirmed(fromMac)) {
        addMac(confirmedSet, fromMac);
        LOG_W(TAG, "onConfirmReceived from=%s count=%zu",
              MacToString(fromMac), confirmedSet.size());
    }
}

void ShootoutManager::recordName(const uint8_t* mac, const char* name) {
    if (name == nullptr) return;
    char buf[kNameLength + 1];
    strncpy(buf, name, kNameLength);
    buf[kNameLength] = '\0';
    if (buf[0] == '\0') return;
    for (auto& entry : names) {
        if (memcmp(entry.mac.data(), mac, 6) == 0) {
            entry.name = buf;
            return;
        }
    }
    NameEntry e;
    memcpy(e.mac.data(), mac, 6);
    e.name = buf;
    names.push_back(std::move(e));
}

std::string ShootoutManager::getNameForMac(const uint8_t* mac) const {
    for (const auto& entry : names) {
        if (memcmp(entry.mac.data(), mac, 6) == 0) return entry.name;
    }
    char fallback[4];
    snprintf(fallback, sizeof(fallback), "%02X", mac[5]);
    return fallback;
}

bool ShootoutManager::hasConfirmed(const uint8_t* mac) const {
    return containsMac(confirmedSet, mac);
}

bool ShootoutManager::allMembersConfirmed(
    const std::vector<std::array<uint8_t, 6>>& members) const {
    // A roster below the floor would run a solo tournament this device wins the
    // instant it starts. sync()'s short-roster timeout bounds the wait.
    if (members.size() < MIN_PARTICIPANTS) return false;
    for (const auto& m : members) {
        if (!hasConfirmed(m.data())) return false;
    }
    return true;
}

std::array<uint8_t, 6> ShootoutManager::getCoordinatorMac() const {
    return coordinatorMac;
}

bool ShootoutManager::headsRing() const {
    return rdc != nullptr && rdc->getChainRole() == ChainRole::RING;
}

const uint8_t* ShootoutManager::ringHead() const {
    if (headsRing()) return wirelessManager->getMacAddress();
    if (rdc == nullptr || !rdc->isInRing()) return nullptr;
    return rdc->getHeadMac();
}

bool ShootoutManager::isCoordinator() const {
    const uint8_t* selfMac = wirelessManager->getMacAddress();
    if (selfMac == nullptr) return false;
    auto coord = getCoordinatorMac();
    return memcmp(coord.data(), selfMac, 6) == 0;
}

void ShootoutManager::generateBracket() {
    bracket = confirmedSet;
    // std::random_device is deterministic under newlib on ESP32, so seed
    // from platform clock XOR self-MAC to get real variation.
    unsigned long seed = 0;
    auto* clk = SimpleTimer::getPlatformClock();
    if (clk != nullptr) seed = clk->milliseconds();
    const uint8_t* selfMac = wirelessManager->getMacAddress();
    if (selfMac != nullptr) {
        for (int i = 0; i < 6; i++) {
            seed ^= static_cast<unsigned long>(selfMac[i]) << ((i % 4) * 8);
        }
    }
    std::mt19937 rng(seed);
    std::shuffle(bracket.begin(), bracket.end(), rng);
    currentRound = bracket;
}

void ShootoutManager::primeMatchManagerForMatch() {
    if (!matchManager) return;
    if (!isLocalDuelist()) return;

    // Role-for-this-match from MAC ordering: both sides compute the same
    // ordering so the hunter_draw_time and bounty_time slots in MatchManager
    // are written by exactly one duelist each. It goes to the bout, never to the
    // Player: the standing role outlives the bout and answers other questions.
    const uint8_t* selfMac = wirelessManager->getMacAddress();
    bool localIsHunterForMatch = selfMac != nullptr &&
                                 memcmp(selfMac, opponentMac.data(), 6) < 0;

    char matchId[IdGenerator::UUID_BUFFER_SIZE];
    deriveShootoutMatchId(currentMatchIndex, matchId, sizeof(matchId));
    LOG_W(TAG, "primeMatchManagerForMatch matchIndex=%d localHunter=%d",
          currentMatchIndex, localIsHunterForMatch);
    matchManager->initializeShootoutMatch(matchId, opponentMac.data(),
                                          localIsHunterForMatch);
}

void ShootoutManager::drawBracket() {
    // Only the device that draws leaves the proposal. A member has nothing to show
    // until a bracket reaches it and the bracket is what moves it, so no state
    // means "in the reveal holding nothing" — and a device still in the proposal
    // is still confirming, which is what gets it counted if the head moves to it.
    // Whoever heads the ring at this moment draws, and authoring the bracket is
    // what makes it the coordinator for the rest of the tournament.
    const uint8_t* selfMac = wirelessManager->getMacAddress();
    if (!headsRing() || selfMac == nullptr) return;
    phase = Phase::BRACKET_REVEAL;
    bracketRevealTimer.setTimer(kBracketRevealMs);
    memcpy(coordinatorMac.data(), selfMac, 6);
    generateBracket();
    sendBracketToPeers();
}

std::vector<uint8_t> ShootoutManager::buildMacListPacket(
    ShootoutCmd cmd, uint8_t seqId,
    const std::vector<std::array<uint8_t, 6>>& macs) const {
    std::vector<uint8_t> packet;
    packet.push_back(static_cast<uint8_t>(cmd));
    packet.push_back(seqId);
    // The roster is the RDC's 64 plus self, so it can land one over what the
    // decoder accepts — and an over-long frame is dropped by every receiver, not
    // just the members past the cap. Truncating keeps the ring running.
    size_t count = macs.size();
    if (count > MAX_BRACKET_SIZE) {
        LOG_E(TAG, "mac list %zu over cap %u; truncating", count,
              static_cast<unsigned>(MAX_BRACKET_SIZE));
        count = MAX_BRACKET_SIZE;
    }
    packet.push_back(static_cast<uint8_t>(count));
    for (size_t i = 0; i < count; i++) {
        packet.insert(packet.end(), macs[i].begin(), macs[i].end());
    }
    return packet;
}

void ShootoutManager::sendBracketToPeers() {
    if (bracket.empty()) return;
    lastBracketSeqId = nextSeqId();
    std::vector<uint8_t> packet =
        buildMacListPacket(ShootoutCmd::BRACKET, lastBracketSeqId, bracket);
    sendReliablyToPeers(bracket, lastBracketSeqId, packet.data(), packet.size());
}

void ShootoutManager::abortTournament() {
    // A tournament that reached its winner is over, not stuck. A late
    // abandonment from a fan-out that outlived the final match must not tear
    // down the standings — and the ABORT would be applied ring-wide, wiping the
    // winner screen on every device.
    if (isTerminalPhase()) return;
    LOG_W(TAG, "abortTournament from phase=%d", static_cast<int>(phase));

    // Copied before resetToIdle clears bracket and confirmedSet.
    const std::vector<std::array<uint8_t, 6>> targets =
        bracket.empty() ? confirmedSet : bracket;

    resetToIdle();
    phase = Phase::ABORTED;

    // Reliable rather than rebroadcast on a timer like RING_CLOSED and CONFIRM:
    // this device has already left the tournament and has nothing to rebroadcast
    // from. Load-bearing on the abandonment path, where the ring is still closed
    // and no member's own ring-break guard will ever fire.
    uint8_t packet[2];
    packet[0] = static_cast<uint8_t>(ShootoutCmd::ABORT);
    packet[1] = nextSeqId();
    sendReliablyToPeers(targets, packet[1], packet, sizeof(packet));
    // Only if one actually went out. A ring of one names no recipients, so there
    // is no group to spare and a seqId recorded here would spare a later frame
    // that happens to reuse it.
    terminalFanOutSeqId = getPendingAckCount(packet[1]) > 0 ? packet[1] : 0;
}

void ShootoutManager::sendLocalConfirm() {
    // [cmd, seq, 6-byte MAC, kNameLength-byte null-padded name]
    uint8_t payload[2 + 6 + kNameLength];
    payload[0] = static_cast<uint8_t>(ShootoutCmd::CONFIRM);
    payload[1] = 0;
    const uint8_t* selfMac = wirelessManager->getMacAddress();
    memcpy(&payload[2], selfMac, 6);
    memset(&payload[8], 0, kNameLength);
    if (player != nullptr) {
        const std::string& n = player->getName();
        size_t copyLen = n.size() < kNameLength ? n.size() : kNameLength;
        memcpy(&payload[8], n.data(), copyLen);
    }

    broadcastToRing(getLoopMembers(), payload, sizeof(payload));
    confirmRebroadcastTimer.setTimer(kConfirmRebroadcastMs);
}

void ShootoutManager::sync() {
    // No event means "the ring opened": a device still holding the peer on its
    // other jack cannot tell a ring-open from a chain getting shorter, so only
    // polling the ring flag sees it. The phase test sits inside the condition
    // because heldFor has to be sampled every tick or its window ages unwatched.
    const bool ringBrokeDuringTournament = active() && !isTerminalPhase() &&
                                           rdc != nullptr && !rdc->isInRing();
    if (ringBreakDebounce.heldFor(ringBrokeDuringTournament, LOOP_BREAK_DEBOUNCE_MS)) {
        abortTournament();
    }

    if (phase == Phase::PROPOSAL) {
        // ringMembers is the tournament's roster, and it only grows. Ring detection
        // can drop a member for a moment while the cables stay put, and drawing off
        // the pruned roster would seat a bracket without it; a member that really
        // leaves breaks the ring, which aborts. A member whose announce to the head
        // was still in flight at closure appears here for the first time. Grown once
        // per tick, so everything below asks the same roster the same question.
        for (const auto& m : getLoopMembers()) {
            if (!containsMac(ringMembers, m.data())) ringMembers.push_back(m);
        }
        // Read once into a local: abortTournament() below empties ringMembers, and
        // everything after it should be answering the roster as this tick found it.
        const std::vector<std::array<uint8_t, 6>> members = ringMembers;
        const bool everyoneIn = allMembersConfirmed(members);
        const uint8_t* selfMac = wirelessManager->getMacAddress();
        const bool confirmedLocally = selfMac != nullptr && hasConfirmed(selfMac);

        // The head announces the roster, and sendRingClosed arms the timer that
        // paces the repeats. Two things need announcing: a member that missed the
        // closure frame is sitting in Idle with no roster to poll, and a device that
        // ring detection settled on only after the proposal started has never sent
        // one at all — hence the never-armed case. No ack needed: a repeat is a
        // no-op once the member is out of Phase::IDLE.
        const bool neverAnnounced = !ringClosedRebroadcastTimer.isRunning();
        if (headsRing() && (neverAnnounced ||
                            (ringClosedRebroadcastTimer.expired() && !everyoneIn))) {
            sendRingClosed();
        }

        // Seeing every confirm says nothing about whether the head has: it may
        // have dropped this device's frame. Only the bracket proves it counted.
        if (confirmRebroadcastTimer.expired() && confirmedLocally) {
            sendLocalConfirm();
        }

        // A roster below the floor never satisfies everyoneIn, so without this the
        // head waits out PROPOSAL_TIMEOUT_MS to reach the same answer a great deal
        // later. Held over a window because the roster is still filling just after a
        // ring closes, and gated on a local confirm — the same gate the bound below
        // carries, so an untouched self-cabled device sits idle rather than flashing.
        const bool ringTooSmallToPlay =
            headsRing() && confirmedLocally && members.size() < MIN_PARTICIPANTS;
        if (shortRosterDebounce.heldFor(ringTooSmallToPlay, SHORT_ROSTER_TIMEOUT_MS)) {
            // Lands in ABORTED like every other giving-up path, so the player
            // gets the same screen. Nothing goes out on the wire: the only
            // recipient set a sub-floor roster can produce is this device alone,
            // and a fan-out naming nobody sends nothing.
            LOG_E(TAG, "ring has too few participants to draw a bracket; aborting");
            abortTournament();
        } else if (confirmedLocally && proposalTimer.expired()) {
            // Nothing else ends this wait: a roster name for a device that has left
            // answers nothing, and the ring is intact so the break guard stays quiet.
            // Gated on a local confirm for the same reason the short-roster abort is —
            // an untouched ring has to sit idle rather than flash ABORTED, and since
            // this device re-proposes off its own standing latch, aborting one it
            // never joined would loop forever.
            LOG_E(TAG, "no roster this ring can complete; aborting");
            abortTournament();
        } else if (everyoneIn) {
            // Drawing is a standing duty of whoever heads the ring, not a chance
            // taken on the CONFIRM that completes the proposal: the head can be
            // unlatched for the tick that completes it, and the device that is head
            // by the next tick draws instead. drawBracket() is a no-op elsewhere.
            drawBracket();
        }
    }

    // The ending is the one word a member cannot do without, and giving up on it is
    // the one abandonment nobody notices: this device is already terminal, so it
    // waits on nothing while the member waits forever. So the ending repeats for as
    // long as the winner is on screen, the way the roster and the confirms repeat —
    // a member that missed the fan-out is repaired by the next copy, whatever the
    // loss count, with no wall clock and no abandonment to notice.
    if (phase == Phase::ENDED && isCoordinator() && endingRebroadcastTimer.expired()) {
        reannounceEnding();
    }

    // Every command family retransmits and abandons here; which one gave up is
    // read off the frame in onCommandAbandoned.
    resender.sync();

    maybeStartNextMatch();
}

std::pair<std::array<uint8_t,6>, std::array<uint8_t,6>>
ShootoutManager::getCurrentMatchPair() const {
    if (currentMatchIndex < 0) return {};
    return {currentDuelistA, currentDuelistB};
}

std::vector<uint8_t> ShootoutManager::buildMatchStartPacket(int matchIndex) const {
    std::vector<uint8_t> packet;
    packet.push_back(static_cast<uint8_t>(ShootoutCmd::MATCH_START));
    packet.push_back(lastMatchStartSeqId);
    const std::array<uint8_t, 6>& a = currentRound[matchIndex * 2];
    const std::array<uint8_t, 6>& b = currentRound[matchIndex * 2 + 1];
    packet.insert(packet.end(), a.begin(), a.end());
    packet.insert(packet.end(), b.begin(), b.end());
    packet.push_back(static_cast<uint8_t>(matchIndex));
    return packet;
}

void ShootoutManager::sendMatchStartToPeers(int matchIndex) {
    lastMatchStartSeqId = nextSeqId();

    auto packet = buildMatchStartPacket(matchIndex);
    const uint8_t* selfMac = wirelessManager->getMacAddress();
    const std::array<uint8_t, 6>& a = currentRound[matchIndex * 2];
    const std::array<uint8_t, 6>& b = currentRound[matchIndex * 2 + 1];
    bool sameMatch = isSameMatch(matchIndex, a.data(), b.data());
    currentDuelistA = a;
    currentDuelistB = b;
    currentMatchIndex = matchIndex;
    if (!sameMatch) {
        reportedLocalWin = false;
    }
    if (!sameMatch && isLocalDuelist() && selfMac != nullptr) {
        const uint8_t* opp = (memcmp(selfMac, a.data(), 6) == 0) ? b.data() : a.data();
        memcpy(opponentMac.data(), opp, 6);
        primeMatchManagerForMatch();
    }
    sendReliablyToPeers(bracket, lastMatchStartSeqId, packet.data(), packet.size());
}

bool ShootoutManager::isSameMatch(int matchIndex, const uint8_t* a, const uint8_t* b) const {
    return matchIndex == currentMatchIndex && phase == Phase::MATCH_IN_PROGRESS && memcmp(currentDuelistA.data(), a, 6) == 0 && memcmp(currentDuelistB.data(), b, 6) == 0;
}

void ShootoutManager::maybeStartNextMatch() {
    if (!isCoordinator()) return;
    // Nobody moves on until the whole bracket has it.
    if (getPendingAckCount(lastBracketSeqId) > 0) return;
    if (phase != Phase::BRACKET_REVEAL && phase != Phase::BETWEEN_MATCHES) return;
    if (phase == Phase::BRACKET_REVEAL && !bracketRevealTimer.expired()) return;
    currentMatchIndex++;
    int pairEnd = currentMatchIndex * 2 + 1;
    if (pairEnd >= static_cast<int>(currentRound.size())) {
        std::vector<std::array<uint8_t, 6>> survivors;
        for (const auto& m : currentRound) {
            if (!isEliminated(m.data())) survivors.push_back(m);
        }
        if (survivors.empty()) {
            // Reachable: both duelists of one bout can report a win, eliminating
            // each other. currentRound starts as the bracket and only ever
            // narrows to survivors, so nobody is left anywhere to crown.
            LOG_E(TAG, "no survivor to crown; aborting");
            abortTournament();
            return;
        }
        if (survivors.size() == 1) {
            sendTournamentEndToPeers(survivors[0].data());
            return;
        }
        LOG_W(TAG, "advancing round: %zu survivors -> %zu",
              currentRound.size(), survivors.size());
        currentRound = survivors;
        currentMatchIndex = 0;
    }
    sendMatchStartToPeers(currentMatchIndex);
    phase = Phase::MATCH_IN_PROGRESS;
}

void ShootoutManager::onBracketReceived(
    const uint8_t* fromMac, const std::vector<std::array<uint8_t, 6>>& offeredBracket,
    uint8_t seqId) {
    const uint8_t* selfMac = wirelessManager->getMacAddress();
    // BRACKET is a broadcast, so a tournament two rings away lands here too, and
    // the roster is the only thing that says whether this one is ours.
    if (!containsMac(offeredBracket, selfMac)) return;
    // A tournament this device has already ended stays ended. The coordinator can
    // still be retransmitting to some other silent member, and nothing below
    // tells that retransmit from a new bracket.
    if (isTerminalPhase()) return;
    // The bracket to take is the one the ring's head drew, and once one is held its
    // author runs the tournament: a head that moves mid-tournament hands nothing
    // over. A refusal is silent, and the sender reads that as a member gone quiet:
    // its retries run out and it aborts its whole tournament (onCommandAbandoned),
    // which is why the no-bracket case admits whatever the ring's own head sends.
    const uint8_t* author = bracket.empty() ? ringHead() : coordinatorMac.data();
    if (author == nullptr || memcmp(fromMac, author, 6) != 0) return;
    // Every member hears the retries meant for a silent one. The ack is owed —
    // refusing our own coordinator makes us the member those retries give up on —
    // but the bracket already in hand is not re-adopted: that would rewind a match
    // in progress. Matched on content rather than against a remembered seqId: the
    // sender's counter is a byte that wraps and restarts at 1 when it reboots, so a
    // repeat and a fresh frame can carry the same id. A different bracket from the
    // same coordinator is a new tournament, and joining it is how a member it gave
    // up on gets back in.
    if (bracket == offeredBracket) {
        sendShootoutAck(ShootoutCmd::BRACKET, seqId, coordinatorMac.data());
        return;
    }
    // Adopting a bracket is the one way into a tournament that runs no reset, so
    // what the last one left behind is retired here instead: an eliminated set would
    // drop every MATCH_START naming a device it still has as out, and a match index
    // would start this tournament part-way through the last one's bracket.
    terminalFanOutSeqId = 0;
    eliminated.clear();
    currentMatchIndex = -1;
    reportedLocalWin = false;
    matchResultResentIndex = -1;
    bracket = offeredBracket;
    currentRound = offeredBracket;
    memcpy(coordinatorMac.data(), fromMac, 6);
    phase = Phase::BRACKET_REVEAL;
    sendShootoutAck(ShootoutCmd::BRACKET, seqId, coordinatorMac.data());
}

void ShootoutManager::onMatchStartReceived(
    const uint8_t* fromMac, const uint8_t* duelistA, const uint8_t* duelistB,
    uint8_t matchIndex, uint8_t seqId) {
    if (isCoordinator()) return;
    if (!isFromCoordinator(fromMac)) return;
    // A tournament this device has already ended stays ended: TOURNAMENT_END
    // leaves the anchor and the bracket standing, so every other gate below
    // would admit a fresh-seqId bout into a finished tournament.
    if (isTerminalPhase()) return;
    // Admitted on the sender, so the ack is owed however the payload reads.
    sendShootoutAck(ShootoutCmd::MATCH_START, seqId, coordinatorMac.data());
    if (!containsMac(bracket, duelistA) || !containsMac(bracket, duelistB)) {
        LOG_E(TAG, "MATCH_START from coordinator names a duelist outside our bracket");
        return;
    }
    // A bout whose loser is already out has been played. maybeStartNextMatch
    // waits only on the BRACKET fan-out, never on MATCH_START's, so the
    // coordinator can announce match N+1 while match N's fan-out is still
    // retrying a member that went quiet. isSameMatch is false for that late N
    // frame, so without this it would drag the member back into a bout it
    // already finished and re-prime it against an opponent it already beat.
    if (isEliminated(duelistA) || isEliminated(duelistB)) return;
    // A repeat of the match in progress. Deduplicated by content rather than
    // against a remembered seqId: the coordinator's counter is a byte that wraps
    // and restarts at 1 when it reboots, so a repeat and a fresh frame can carry
    // the same id.
    if (isSameMatch(matchIndex, duelistA, duelistB)) return;
    currentMatchIndex = matchIndex;
    memcpy(currentDuelistA.data(), duelistA, 6);
    memcpy(currentDuelistB.data(), duelistB, 6);
    phase = Phase::MATCH_IN_PROGRESS;
    reportedLocalWin = false;
    const uint8_t* selfMac = wirelessManager->getMacAddress();
    if (isLocalDuelist() && selfMac != nullptr) {
        const uint8_t* opp = (memcmp(selfMac, duelistA, 6) == 0) ? duelistB : duelistA;
        memcpy(opponentMac.data(), opp, 6);
        primeMatchManagerForMatch();
    }
}

void ShootoutManager::sendShootoutAck(ShootoutCmd cmd, uint8_t seqId, const uint8_t* toMac) {
    ShootoutAckPayload ack{cmd, seqId};
    wirelessManager->sendEspNowData(toMac, PktType::kShootoutCommandAck,
                                    reinterpret_cast<uint8_t*>(&ack), sizeof(ack));
}

bool ShootoutManager::isEliminated(const uint8_t* mac) const {
    return containsMac(eliminated, mac);
}

void ShootoutManager::applyMatchResult(const uint8_t* winner, const uint8_t* loser,
                                       bool endsCurrentBout) {
    addMac(eliminated, loser);
    if (!endsCurrentBout) return;
    // A duelist still mounted when the tournament ended resolves its own bout and
    // reports it. Walking ENDED back reopens a round that no longer exists;
    // walking ABORTED back is worse, because the abort edge is read later in the
    // same tick and would then read false, stranding this device in a tournament
    // every other member has left.
    if (isTerminalPhase()) return;
    phase = Phase::BETWEEN_MATCHES;
}

std::vector<uint8_t> ShootoutManager::buildMatchResultPacket(
    const uint8_t* winner, const uint8_t* loser, uint8_t matchIndex) const {
    std::vector<uint8_t> packet;
    packet.push_back(static_cast<uint8_t>(ShootoutCmd::MATCH_RESULT));
    packet.push_back(lastMatchResultSeqId);
    packet.insert(packet.end(), winner, winner + 6);
    packet.insert(packet.end(), loser, loser + 6);
    packet.push_back(matchIndex);
    return packet;
}

void ShootoutManager::sendMatchResultToPeers(
    const uint8_t* winner, const uint8_t* loser, uint8_t matchIndex) {
    lastMatchResultSeqId = nextSeqId();
    auto packet = buildMatchResultPacket(winner, loser, matchIndex);
    // Targets bracket, not confirmedSet. Both reach eliminated players — only
    // currentRound shrinks — but confirmedSet is each device's own tally of the
    // CONFIRMs it happened to hear, and those are unacked broadcasts sent once
    // per press. A follower that missed the coordinator's would never track it
    // as a recipient, so its result could never abandon against the coordinator
    // and the recovery below could never fire. The bracket is the coordinator's
    // own roster, acked on arrival.
    sendReliablyToPeers(bracket, lastMatchResultSeqId, packet.data(), packet.size());
}

void ShootoutManager::reportLocalWin() {
    const uint8_t* selfMac = wirelessManager->getMacAddress();
    if (selfMac == nullptr) return;
    if (reportedLocalWin) return;
    reportedLocalWin = true;
    LOG_W(TAG, "reportLocalWin matchIndex=%d", currentMatchIndex);
    sendMatchResultToPeers(selfMac, opponentMac.data(), static_cast<uint8_t>(currentMatchIndex));
    applyMatchResult(selfMac, opponentMac.data());
    // The round advances on the next sync(), not from here. On the timeout path
    // this is called from Duel::onStateLoop, and that state dismounts later in
    // the same tick through clearCurrentMatch() — anything primed for the next
    // bout from here is torn down by the bout that is still being left.
}

void ShootoutManager::onMatchResultReceived(
    const uint8_t* winner, const uint8_t* loser,
    uint8_t matchIndex, uint8_t seqId, const uint8_t* fromMac) {
    // A result is fanned out by whichever duelist won it, so the sender being in
    // our bracket is what says the frame is ours. A foreign ring's result is
    // refused before the ack for the same reason ABORT is: an ack is a unicast,
    // and a unicast takes a slot in the radio's 20-entry peer table, evicting
    // whatever sat there longest.
    if (!containsMac(bracket, fromMac)) return;
    // Always ack so the sender stops retrying, even when this is a duplicate.
    sendShootoutAck(ShootoutCmd::MATCH_RESULT, seqId, fromMac);
    if (!containsMac(bracket, winner) || !containsMac(bracket, loser)) {
        LOG_E(TAG, "MATCH_RESULT from ring member names a device outside our bracket");
        return;
    }
    // Dedup by loser-MAC rather than seqId: non-coord senders have independent
    // seq counters, but each loser is eliminated exactly once per tournament.
    if (isEliminated(loser)) {
        return;
    }
    LOG_W(TAG, "onMatchResultReceived matchIndex=%u", matchIndex);
    // Record the elimination, but only let a result end the bout it belongs to.
    // A result can arrive late — its sender re-sends when the coordinator misses
    // one — and a device that has since been paired into a newer match would
    // otherwise be pulled out of it mid-duel by a result about the previous one.
    const bool namesCurrentBout =
        currentMatchIndex < 0 || static_cast<int>(matchIndex) == currentMatchIndex;
    applyMatchResult(winner, loser, namesCurrentBout);
}

void ShootoutManager::sendTournamentEndToPeers(const uint8_t* winner) {
    LOG_W(TAG, "tournamentEnd winner=%s", MacToString(winner));
    lastTournamentEndSeqId = nextSeqId();
    uint8_t packet[8];
    packet[0] = static_cast<uint8_t>(ShootoutCmd::TOURNAMENT_END);
    packet[1] = lastTournamentEndSeqId;
    memcpy(&packet[2], winner, 6);
    // Targets confirmedSet rather than bracket: eliminated players need the
    // tournament-end transition or they stall in BETWEEN_MATCHES.
    sendReliablyToPeers(confirmedSet, lastTournamentEndSeqId, packet, sizeof(packet));
    terminalFanOutSeqId = lastTournamentEndSeqId;
    memcpy(tournamentWinner.data(), winner, 6);
    endingRebroadcastTimer.setTimer(kConfirmRebroadcastMs);
    phase = Phase::ENDED;
}

void ShootoutManager::reannounceEnding() {
    // seqId 0: unreliable on purpose. A reliable repeat would hold the terminal
    // fan-out open, and TerminalFanOutsDone holds every later proposal behind it.
    uint8_t packet[8];
    packet[0] = static_cast<uint8_t>(ShootoutCmd::TOURNAMENT_END);
    packet[1] = 0;
    memcpy(&packet[2], tournamentWinner.data(), 6);
    broadcastToRing(confirmedSet, packet, sizeof(packet));
    endingRebroadcastTimer.setTimer(kConfirmRebroadcastMs);
}

void ShootoutManager::onTournamentEndReceived(const uint8_t* fromMac,
                                              const uint8_t* winner, uint8_t seqId) {
    if (!isFromCoordinator(fromMac)) return;
    // Admitted on the sender, so the ack is owed however the payload reads — except
    // for seqId 0, which marks the unreliable repeat nobody is waiting on.
    if (seqId != 0) sendShootoutAck(ShootoutCmd::TOURNAMENT_END, seqId, coordinatorMac.data());
    if (!containsMac(bracket, winner)) {
        LOG_E(TAG, "TOURNAMENT_END from coordinator names a winner outside our bracket");
        return;
    }
    memcpy(tournamentWinner.data(), winner, 6);
    phase = Phase::ENDED;
}

void ShootoutManager::onAbortReceived(const uint8_t* fromMac, uint8_t seqId) {
    // One broadcast reaches every device in radio range, other rings included.
    // The ack stays behind this filter because a unicast takes one of the radio's
    // 20 peer slots, evicting whatever sat there longest. A follower that already
    // aborted fails it too, its rosters cleared, so the sender spends retries on a
    // device that has stopped; a still-cabled head keeps answering, its roster
    // coming live from the RDC.
    if (!isRingMember(fromMac)) return;
    // Addressed to fromMac because any ring member may abort, not just the
    // coordinator.
    if (seqId != 0) sendShootoutAck(ShootoutCmd::ABORT, seqId, fromMac);
    // ENDED is refused here too, and reachably: a member that missed
    // TOURNAMENT_END is still in BETWEEN_MATCHES, so a cable pulled after the
    // winner appears sends ABORT to devices already showing the result.
    if (isTerminalPhase() || phase == Phase::IDLE) return;
    resetToIdle();
    phase = Phase::ABORTED;
}

std::vector<std::array<uint8_t, 6>> ShootoutManager::buildLoopMemberSet() const {
    // The RDC serves a roster only where it is the authority, which inside a ring
    // is the head that latched it. Every other member holds the copy that head
    // sent with RING_CLOSED.
    if (rdc == nullptr || rdc->getChainRole() != ChainRole::RING) return ringMembers;

    std::vector<std::array<uint8_t, 6>> members = rdc->getChainMembers();
    // getChainMembers() enumerates the devices that announced to the head, never
    // the head itself, and the head is a participant like any other.
    const uint8_t* selfMac = wirelessManager->getMacAddress();
    if (selfMac != nullptr) addMac(members, selfMac);
    return members;
}
