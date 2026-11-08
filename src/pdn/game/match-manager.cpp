#include "game/match-manager.hpp"
#include <ArduinoJson.h>
#include "device/drivers/logger.hpp"
#include "wireless/match-packet.hpp"
#include "game/shootout-manager.hpp"
#include "id-generator.hpp"
#include <optional>
#include <cstring>

static constexpr const char* PREF_COUNT_KEY = "count";
static constexpr const char* PREF_MATCH_KEY  = "match_";
static constexpr uint8_t     MAX_MATCHES      = 255;
static constexpr long MIN_EFFECTIVE_DRAW_TIME_MS = -300;
static constexpr unsigned long MAX_DRAW_TIME_MS = 60000;
static constexpr int MAX_BUTTON_MASHER_COUNT = 255;

static const char* const MATCH_MANAGER_TAG = "MATCH_MANAGER";

MatchManager::MatchManager(WirelessManager* wirelessManager)
    : player(nullptr)
    , storage(nullptr)
    , resender(wirelessManager)
    , matchChannel(wirelessManager, &resender, PktType::kMatchCommand, {}) {
    resender.setAbandonCallback(
    [this](PktType, uint8_t, const uint8_t* targetMac, const uint8_t*, size_t) {
        if (!activeMatchState.match.has_value() || !activeMatchState.awaitingAckDelivery) return;
        if (memcmp(targetMac, activeMatchState.opponentMac.data(), 6) == 0) {
            clearCurrentMatch();
        }
    });
    matchChannel.onReceive([this](const uint8_t* fromMac, const MatchPacket& p) {
        MatchCommand command(fromMac, p.command, p.matchId, p.playerId, p.playerDrawTime,
                             p.isHunter, p.rawDrawTime);
        listenForMatchEvents(command);
    });
    matchChannel.setOnDelivered([this](uint8_t, const uint8_t* toMac) {
        if (!activeMatchState.match.has_value() || !activeMatchState.awaitingAckDelivery) return;
        if (memcmp(toMac, activeMatchState.opponentMac.data(), 6) == 0) {
            activeMatchState.awaitingAckDelivery = false;
            activeMatchState.matchIsReady = true;
        }
    });
}

void MatchManager::sync() {
    resender.sync();
}

void MatchManager::sendCommand(const uint8_t* mac, int command, const char* matchId, long drawTime,
                               bool isHunter, unsigned long rawDrawTime) {
    MatchPacket packet{};
    packet.command = command;
    packet.playerDrawTime = drawTime;
    packet.rawDrawTime = rawDrawTime;
    packet.isHunter = isHunter;
    IdGenerator::copyId(packet.matchId, matchId);
    IdGenerator::copyId(packet.playerId, player->getUserID().c_str());

    LOG_I(MATCH_MANAGER_TAG, "Sending command %i to %s", command, MacToString(mac));
    matchChannel.sendReliable(mac, packet);
}

MatchManager::~MatchManager() {
    if (storage) {
        storage->end();
    }
}

void MatchManager::setBoostProvider(std::function<unsigned long()> provider) {
    boostProvider_ = provider;
}

void MatchManager::setRemoteDeviceCoordinator(RemoteDeviceCoordinator* rdc) {
    rdc_ = rdc;
}

void MatchManager::setShootoutManager(ShootoutManager* shootoutManager) {
    shootoutManager_ = shootoutManager;
}

ShootoutManager* MatchManager::getShootoutManager() const {
    return shootoutManager_;
}

bool MatchManager::currentMatchIsShootout() const {
    return activeMatchState.match.has_value() &&
           strncmp(activeMatchState.match->getMatchId(), kShootoutMatchIdPrefix,
                   sizeof(kShootoutMatchIdPrefix) - 1) == 0;
}

void MatchManager::clearShootoutMatch() {
    if (!currentMatchIsShootout()) return;
    clearCurrentMatch();
}

void MatchManager::clearCurrentMatch() {
    if (!activeMatchState.match) return;
    LOG_I(MATCH_MANAGER_TAG, "Clearing current match");
    // Keep matchChannel retrying only when a local DRAW_RESULT or NEVER_PRESSED was sent.
    if (!activeMatchState.hasPressedButton && !activeMatchState.gracePeriodExpiredNoResult) {
        matchChannel.cancel(activeMatchState.opponentMac.data());
    }
    activeMatchState = ActiveMatchState{};
}

void MatchManager::primeMatch(const char* matchId, const uint8_t* opponentMac,
                              bool localIsHunter) {
    activeMatchState.localIsHunter = localIsHunter;
    activeMatchState.match.emplace(matchId, player->getUserID().c_str(), localIsHunter);
    memcpy(activeMatchState.opponentMac.data(), opponentMac, 6);
}

void MatchManager::initializeMatch(uint8_t* opponentMac) {
    if (activeMatchState.match.has_value()) {
        return;
    }

    auto* clock = SimpleTimer::getPlatformClock();
    LOG_D(MATCH_MANAGER_TAG, "TIMING initializeMatch T=%lu", clock ? clock->milliseconds() : 0UL);

    primeMatch(IdGenerator::getInstance().generateId(), opponentMac, player->isHunter());
    sendCommand(activeMatchState.opponentMac.data(), MatchCmd::SEND_MATCH_ID,
                activeMatchState.match->getMatchId(), 0, activeMatchState.localIsHunter);
}

void MatchManager::initializeShootoutMatch(const char* matchId, uint8_t* opponentMac,
                                           bool localIsHunter) {
    // A repeat MATCH_START names a match that may already be mounted; the live one
    // wins, since re-priming would reset a draw already in progress.
    if (activeMatchState.match.has_value()) return;

    auto* clock = SimpleTimer::getPlatformClock();
    LOG_D(MATCH_MANAGER_TAG, "TIMING initializeShootoutMatch T=%lu",
          clock ? clock->milliseconds() : 0UL);

    primeMatch(matchId, opponentMac, localIsHunter);
    activeMatchState.matchIsReady = true;
}

void MatchManager::receiveMatch(const char* matchId, const char* opponentId, bool opponentIsHunter,
                                uint8_t* opponentMac) {
    if (activeMatchState.match.has_value()) {
        return;
    }

    primeMatch(matchId, opponentMac, player->isHunter());
    if (opponentIsHunter) {
        activeMatchState.match->setHunterId(opponentId);
    } else {
        activeMatchState.match->setBountyId(opponentId);
    }

    auto* clock = SimpleTimer::getPlatformClock();
    LOG_D(MATCH_MANAGER_TAG, "TIMING receiveMatch-ackSent T=%lu",
          clock ? clock->milliseconds() : 0UL);

    activeMatchState.awaitingAckDelivery = true;
    sendCommand(activeMatchState.opponentMac.data(), MatchCmd::MATCH_ID_ACK,
                activeMatchState.match->getMatchId(), 0, activeMatchState.localIsHunter);
}

void MatchManager::setMatchLocalStartTime(unsigned long localStartTimeMs) {
    activeMatchState.matchLocalStartTime = localStartTimeMs;
}

unsigned long MatchManager::getMatchLocalStartTime() const {
    return activeMatchState.matchLocalStartTime;
}

bool MatchManager::matchResultsAreIn() {
    if (!activeMatchState.match) {
        return false;
    }

    // Final clause is the deadlock escape for both-timed-out matches where the
    // opponent's NEVER_PRESSED packet drops. Gated on !hasPressedButton so a
    // button-press in the same tick as grace expiry still takes the press
    // path (clauses 1/2) — otherwise didWin() would spuriously return false.
    return (activeMatchState.hasReceivedDrawResult && activeMatchState.hasPressedButton) ||
           (activeMatchState.opponentNeverPressed && activeMatchState.hasPressedButton) ||
           (activeMatchState.gracePeriodExpiredNoResult && !activeMatchState.hasPressedButton);
}

void MatchManager::setNeverPressed() { activeMatchState.gracePeriodExpiredNoResult = true; }

bool MatchManager::didWin() {
    if (!activeMatchState.match) {
        return false;
    }

    if (activeMatchState.opponentNeverPressed) {
        return true;
    }
    if (activeMatchState.gracePeriodExpiredNoResult) {
        return false;
    }
    if (!activeMatchState.hunterEffectiveTimeMs.has_value() ||
        !activeMatchState.bountyEffectiveTimeMs.has_value()) {
        return false;
    }

    const long hunterTime = *activeMatchState.hunterEffectiveTimeMs;
    const long bountyTime = *activeMatchState.bountyEffectiveTimeMs;
    return activeMatchState.localIsHunter ? hunterTime < bountyTime : bountyTime < hunterTime;
}

bool MatchManager::finalizeMatch() {
    if (!activeMatchState.match) {
        LOG_E(MATCH_MANAGER_TAG, "Cannot finalize match - no active match or ID mismatch\n");
        return false;
    }

    const long hunterTime = activeMatchState.hunterEffectiveTimeMs.value_or(0);
    const long bountyTime = activeMatchState.bountyEffectiveTimeMs.value_or(0);

    // Snapshot times for the result screen (before clearCurrentMatch wipes the match).
    lastMatchDisplay.myTimeMs = activeMatchState.localIsHunter ? hunterTime : bountyTime;
    lastMatchDisplay.opponentTimeMs = activeMatchState.localIsHunter ? bountyTime : hunterTime;
    lastMatchDisplay.hasData = true;

    const bool hunterWon = (didWin() == activeMatchState.localIsHunter);
    activeMatchState.match->setWinnerIsHunter(hunterWon);

    std::string matchId = activeMatchState.match->getMatchId();

    // Shootout matches are local-ephemeral: no save, no upload.
    if (currentMatchIsShootout()) {
        clearCurrentMatch();
        return true;
    }

    if (appendMatchToStorage(&*activeMatchState.match)) {
        updateStoredMatchCount(getStoredMatchCount() + 1);
        clearCurrentMatch();
        LOG_I(MATCH_MANAGER_TAG, "Successfully finalized match %s\n", matchId.c_str());
        return true;
    }

    LOG_E("PDN", "Failed to finalize match %s\n", matchId.c_str());
    return false;
}

bool MatchManager::getHasReceivedDrawResult() const {
    return activeMatchState.hasReceivedDrawResult;
}

bool MatchManager::getHasPressedButton() const { return activeMatchState.hasPressedButton; }

void MatchManager::setReceivedDrawResult() { activeMatchState.hasReceivedDrawResult = true; }

parameterizedCallbackFunction MatchManager::getMatchButtonPush() { return matchButtonPush; }

void MatchManager::setReceivedButtonPush() { activeMatchState.hasPressedButton = true; }

bool MatchManager::setHunterDrawTime(long effectiveTimeMs, unsigned long rawTimeMs) {
    if (!activeMatchState.match) {
        return false;
    }
    activeMatchState.hunterEffectiveTimeMs = effectiveTimeMs;
    activeMatchState.match->setHunterDrawTime(rawTimeMs);
    return true;
}

bool MatchManager::setBountyDrawTime(long effectiveTimeMs, unsigned long rawTimeMs) {
    if (!activeMatchState.match) {
        return false;
    }
    activeMatchState.bountyEffectiveTimeMs = effectiveTimeMs;
    activeMatchState.match->setBountyDrawTime(rawTimeMs);
    return true;
}

std::string MatchManager::toJson() {
    // Create JSON document with an object at the root
    JsonDocument doc;  // Adjust size based on max matches
    
    // Create a "matches" array within the root object
    JsonArray matchArray = doc["matches"].to<JsonArray>();

    // Read all matches from storage
    uint8_t count = getStoredMatchCount();
    for (uint8_t i = 0; i < count; i++) {
        Match* match = readMatchFromStorage(i);
        if (match) {
            // Add match to array
            std::string matchJson = match->toJson();
            JsonDocument matchDoc;
            deserializeJson(matchDoc, matchJson);
            matchArray.add(matchDoc.as<JsonObject>());
            delete match;
        }
    }

    // Serialize to string
    std::string output;
    serializeJson(doc, output);
    return output;
}

void MatchManager::clearStorage() {
    storage->clear(MATCHES_PREFS_NAMESPACE);
    updateStoredMatchCount(0);
    LOG_I("PDN", "Cleared match storage\n");
}

size_t MatchManager::getStoredMatchCount() {
    return storage->readUChar(MATCHES_PREFS_NAMESPACE, PREF_COUNT_KEY, 0);
}

bool MatchManager::appendMatchToStorage(const Match* match) {
    if (!match) return false;

    uint8_t count = getStoredMatchCount();
    if (count >= MAX_MATCHES) {
        LOG_E("PDN", "Cannot save match - storage full\n");
        return false;
    }

    // Convert match to JSON for storage
    std::string matchJson = match->toJson();
    
    // Create key for this match
    char key[16];
    snprintf(key, sizeof(key), "%s%d", PREF_MATCH_KEY, count);

    LOG_W(MATCH_MANAGER_TAG, "Attempting to save match to storage at key %s (JSON length: %d bytes)", 
            key, matchJson.length());
    LOG_W(MATCH_MANAGER_TAG, "Match JSON: %s", matchJson.c_str());
    
    // Try to check if preferences is working
    if (storage->writeUChar(MATCHES_PREFS_NAMESPACE, "test_key", 123) != 1) {
        LOG_E(MATCH_MANAGER_TAG, "NVS Preference test write failed! Potential hardware or NVS issue");
    } else {
        uint8_t testValue = storage->readUChar(MATCHES_PREFS_NAMESPACE, "test_key", 0);
        LOG_W(MATCH_MANAGER_TAG, "NVS test write/read successful: wrote 123, read %d", testValue);
    }

    // Save match JSON to preferences
    if (storage->write(MATCHES_PREFS_NAMESPACE, key, matchJson) != matchJson.length()) {
        LOG_E(MATCH_MANAGER_TAG, "Failed to save match to storage - key: %s, length: %d", 
                key, matchJson.length());
        
        return false;
    }

    LOG_W(MATCH_MANAGER_TAG, "Successfully saved match to storage at index %d", count);
    return true;
}

void MatchManager::updateStoredMatchCount(uint8_t count) {
    if (storage->writeUChar(MATCHES_PREFS_NAMESPACE, PREF_COUNT_KEY, count) != 1) {
        LOG_E("PDN", "Failed to update match count\n");
    } else {
        LOG_I("PDN", "Updated stored match count to %d\n", count);
    }
}

Match* MatchManager::readMatchFromStorage(uint8_t index) {
    if (index >= getStoredMatchCount()) {
        return nullptr;
    }

    // Create key for this match
    char key[16];
    snprintf(key, sizeof(key), "%s%d", PREF_MATCH_KEY, index);
    
    // Read match JSON from preferences
    std::string matchJson = storage->read(MATCHES_PREFS_NAMESPACE, key, "");
    if (matchJson.empty()) {
        return nullptr;
    }

    // Create and deserialize match
    Match* match = new Match();
    match->fromJson(matchJson);
    return match;
}

parameterizedCallbackFunction MatchManager::getButtonMasher() {
    return buttonMasher;
}

void MatchManager::initialize(Player* player, StorageInterface* storage) {
    this->player = player;
    this->storage = storage;

    matchButtonPush = [](void* ctx) {
        if (!ctx) {
            LOG_E(MATCH_MANAGER_TAG, "Button press handler received null context");
            return;
        }

        auto* clock = SimpleTimer::getPlatformClock();
        if (!clock) {
            LOG_E(MATCH_MANAGER_TAG, "Button press handler called with no platform clock");
            return;
        }
        unsigned long now = clock->milliseconds();

        MatchManager* matchManager = static_cast<MatchManager*>(ctx);
        ActiveMatchState* activeMatchState = &matchManager->activeMatchState;
        Player* player = matchManager->player;

        if(matchManager->getHasPressedButton()) {
            LOG_I(MATCH_MANAGER_TAG, "Button already pressed - skipping");
            return;
        }

        if(!matchManager->getCurrentMatch().has_value()) {
            LOG_E(MATCH_MANAGER_TAG, "No current match - skipping");
            return;
        }

        const unsigned long startTime = matchManager->getMatchLocalStartTime();
        unsigned long reactionTimeMs = now >= startTime ? (now - startTime) : 0;
        if (reactionTimeMs > MAX_DRAW_TIME_MS) {
            reactionTimeMs = MAX_DRAW_TIME_MS;
        }
        unsigned long penaltyMs = activeMatchState->calculateButtonMasherPenalty();
        reactionTimeMs += penaltyMs;
        if (reactionTimeMs > MAX_DRAW_TIME_MS) {
            reactionTimeMs = MAX_DRAW_TIME_MS;
        }

        unsigned long boost = matchManager->boostProvider_ ? matchManager->boostProvider_() : 0;
        const unsigned long maxBoost = static_cast<unsigned long>(-MIN_EFFECTIVE_DRAW_TIME_MS);
        if (boost > maxBoost) {
            boost = maxBoost;
        }
        long boostedTimeMs = static_cast<long>(reactionTimeMs) - static_cast<long>(boost);
        if (boostedTimeMs < MIN_EFFECTIVE_DRAW_TIME_MS) {
            boostedTimeMs = MIN_EFFECTIVE_DRAW_TIME_MS;
        }

        // Snapshot boost and early-press penalty at press time so the result
        // screen can display them even if supporters unplug before the match finalizes.
        matchManager->lastMatchDisplay.boostMs = boost;
        matchManager->lastMatchDisplay.penaltyMs = penaltyMs;

        LOG_I(
        MATCH_MANAGER_TAG, "Button pressed! Reaction time: %ld ms (boost %lu, penalty %lu) for %s",
        boostedTimeMs, boost, penaltyMs, activeMatchState->localIsHunter ? "Hunter" : "Bounty");

        activeMatchState->localIsHunter
        ? matchManager->setHunterDrawTime(boostedTimeMs, reactionTimeMs)
        : matchManager->setBountyDrawTime(boostedTimeMs, reactionTimeMs);

        // Player stats record the raw (unboosted) reaction time — boost is a
        // match-time advantage, not an achievement the player actually made.
        player->addReactionTime(reactionTimeMs);

        LOG_I(MATCH_MANAGER_TAG, "Stored reaction time in MatchManager");

        LOG_I(MATCH_MANAGER_TAG, "Broadcasting DRAW_RESULT to opponent MAC: %s",
              MacToString(activeMatchState->opponentMac.data()));

        // Send both the signed effective draw time (for winner resolution) and the
        // unsigned raw reaction time (for the persisted Match record).
        matchManager->sendCommand(activeMatchState->opponentMac.data(), MatchCmd::DRAW_RESULT,
                                  matchManager->getCurrentMatch()->getMatchId(), boostedTimeMs,
                                  activeMatchState->localIsHunter, reactionTimeMs);

        matchManager->setReceivedButtonPush();
        
        LOG_I(MATCH_MANAGER_TAG, "Reaction time: %lu ms", reactionTimeMs);
    };

    buttonMasher = [](void *ctx) {
        MatchManager* matchManager = static_cast<MatchManager*>(ctx);
        if (matchManager->activeMatchState.buttonMasherCount < MAX_BUTTON_MASHER_COUNT) {
            matchManager->activeMatchState.buttonMasherCount++;
        }
        LOG_I(MATCH_MANAGER_TAG, "Button masher count: %d",
              matchManager->activeMatchState.buttonMasherCount);
    };
}

bool MatchManager::isFromActiveMatchOpponent(const MatchCommand& command) const {
    if (!activeMatchState.match.has_value()) return false;
    if (memcmp(command.wifiMacAddr, activeMatchState.opponentMac.data(), 6) != 0) return false;
    if (strcmp(command.matchId, activeMatchState.match->getMatchId()) != 0) return false;
    return true;
}

void MatchManager::listenForMatchEvents(const MatchCommand& command) {
    LOG_I(MATCH_MANAGER_TAG, "Received command: %d", command.command);

    if (command.command == MatchCmd::SEND_MATCH_ID) {
        // Gate on RDC peering — no active match exists yet, so the sender
        // must be authenticated via the cable-established direct peer.
        LOG_I(MATCH_MANAGER_TAG, "Received SEND_MATCH_ID command from opponent");
        if (!rdc_ || !rdc_->isDirectPeer(command.wifiMacAddr)) {
            LOG_W(MATCH_MANAGER_TAG, "Rejecting SEND_MATCH_ID: sender not an RDC direct peer");
            return;
        }
        bool shootoutActive = shootoutManager_ && shootoutManager_->active();
        if(!shootoutActive && player->isHunter() == command.isHunter) {
            // Echo the sender's matchId/MAC: activeMatchState.match is still empty.
            sendCommand(command.wifiMacAddr, MatchCmd::MATCH_ROLE_MISMATCH, command.matchId, 0,
                        player->isHunter());
            return;
        } else {
            receiveMatch(command.matchId, command.playerId, command.isHunter, const_cast<uint8_t*>(command.wifiMacAddr));
        }
    } else if (command.command == MatchCmd::MATCH_ID_ACK) {
        LOG_I(MATCH_MANAGER_TAG, "Received MATCH_ID_ACK command from opponent");
        if (!isFromActiveMatchOpponent(command)) return;
        if (activeMatchState.matchIsReady || activeMatchState.awaitingAckDelivery) return;
        matchChannel.cancel(activeMatchState.opponentMac.data());
        if (activeMatchState.localIsHunter) {
            activeMatchState.match->setBountyId(command.playerId);
        } else {
            activeMatchState.match->setHunterId(command.playerId);
        }
        auto* clock = SimpleTimer::getPlatformClock();
        LOG_D(MATCH_MANAGER_TAG, "TIMING matchAck-ready T=%lu", clock ? clock->milliseconds() : 0UL);
        activeMatchState.matchIsReady = true;
    } else if (command.command == MatchCmd::MATCH_ROLE_MISMATCH) {
        LOG_I(MATCH_MANAGER_TAG, "Received MATCH_ROLE_MISMATCH command from opponent");
        if (!isFromActiveMatchOpponent(command)) return;
        if (activeMatchState.matchIsReady || activeMatchState.awaitingAckDelivery) return;
        clearCurrentMatch();
        return;
    } else if (command.command == MatchCmd::DRAW_RESULT ||
               command.command == MatchCmd::NEVER_PRESSED) {
        LOG_I(MATCH_MANAGER_TAG, "Received DRAW_RESULT command from opponent");
        if (!isFromActiveMatchOpponent(command)) return;
        if (command.isHunter == activeMatchState.localIsHunter) return;

        const long opponentTime = command.playerDrawTime;
        const unsigned long opponentRawTime = command.rawDrawTime;
        if (opponentTime < MIN_EFFECTIVE_DRAW_TIME_MS) return;
        if (opponentRawTime > MAX_DRAW_TIME_MS) return;
        const long rawAsSigned = static_cast<long>(opponentRawTime);
        if (opponentTime > rawAsSigned) return;
        if (rawAsSigned - opponentTime > -MIN_EFFECTIVE_DRAW_TIME_MS) return;
        if (command.command == MatchCmd::NEVER_PRESSED && opponentTime != rawAsSigned) return;

        LOG_I(MATCH_MANAGER_TAG, "Opponent reaction time: %ld ms", opponentTime);

        command.isHunter ? setHunterDrawTime(opponentTime, opponentRawTime)
                         : setBountyDrawTime(opponentTime, opponentRawTime);

        if (command.command == MatchCmd::NEVER_PRESSED) {
            activeMatchState.opponentNeverPressed = true;
        }

        setReceivedDrawResult();
    } else {
        LOG_W(MATCH_MANAGER_TAG, "Received unexpected command in Match Manager: %d", command.command);
    }
}

void MatchManager::sendNeverPressed(unsigned long pityTime) {
    if (!activeMatchState.match.has_value()) {
        LOG_E(MATCH_MANAGER_TAG, "Cannot send NEVER_PRESSED - no active match");
        return;
    }

    if (pityTime > MAX_DRAW_TIME_MS) {
        pityTime = MAX_DRAW_TIME_MS;
    }
    const long signedPityTime = static_cast<long>(pityTime);

    lastMatchDisplay.boostMs = 0;
    lastMatchDisplay.penaltyMs = 0;
    activeMatchState.localIsHunter ? setHunterDrawTime(signedPityTime, pityTime)
                                   : setBountyDrawTime(signedPityTime, pityTime);
    setNeverPressed();
    // Mirror what gets uploaded: every match contributes a draw time to the server,
    // so on-device "average reaction" should include pity times too.
    player->addReactionTime(pityTime);

    sendCommand(activeMatchState.opponentMac.data(), MatchCmd::NEVER_PRESSED,
                activeMatchState.match->getMatchId(), signedPityTime,
                activeMatchState.localIsHunter, pityTime);
}

bool MatchManager::isMatchReady() const { return activeMatchState.matchIsReady; }