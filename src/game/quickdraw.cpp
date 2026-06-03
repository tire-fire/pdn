#include "../../include/game/quickdraw.hpp"
#include "wireless/peer-comms-types.hpp"
#include "wireless/symbol-wireless-manager.hpp"
#include "device/drivers/logger.hpp"
#include <array>
#include <cstring>

Quickdraw::Quickdraw(Player* player, Device* PDN, QuickdrawWirelessManager* quickdrawWirelessManager, RemoteDebugManager* remoteDebugManager, SymbolWirelessManager* symbolWirelessManager, WirelessTransport* transport): StateMachine(QUICKDRAW_APP_ID) {
    this->player = player;
    this->quickdrawWirelessManager = quickdrawWirelessManager;
    this->symbolWirelessManager = symbolWirelessManager;
    this->remoteDebugManager = remoteDebugManager;
    this->wirelessManager = PDN->getWirelessManager();
    this->matchManager = new MatchManager();
    this->storageManager = PDN->getStorage();
    this->peerComms = PDN->getPeerComms();
    this->remoteDeviceCoordinator = PDN->getRemoteDeviceCoordinator();

    this->chainManager = new ChainManager(player, wirelessManager, remoteDeviceCoordinator);
    if (transport != nullptr) {
        this->chainManager->initialize(transport);
    }
    this->shootoutManager_ = new ShootoutManager(player, wirelessManager, remoteDeviceCoordinator, chainManager);
    if (transport != nullptr) {
        this->shootoutManager_->initialize(transport);
    }
    this->shootoutManager_->setMatchManager(matchManager);
    PDN->setShootoutManager(shootoutManager_);
    matchManager->setShootoutManager(shootoutManager_);

    matchManager->initialize(player, storageManager, quickdrawWirelessManager);
    matchManager->setBoostProvider([this]() -> unsigned long {
        return chainManager ? chainManager->getBoostMs() : 0;
    });
    matchManager->setRemoteDeviceCoordinator(remoteDeviceCoordinator);

    quickdrawWirelessManager->setPacketReceivedCallback(
        std::bind(&MatchManager::listenForMatchEvents, matchManager, std::placeholders::_1)
    );

    quickdrawWirelessManager->setAbandonCallback(
        [this] {
            if (matchManager) matchManager->onReliableSendAbandoned();
        });

    // kChainGameEvent / kChainConfirm / kRoleAnnounce handlers are installed
    // inside ChainManager::initialize, routing inbound bytes through the
    // transport's per-channel dispatcher. Likewise, kShootoutCommand is
    // installed inside ShootoutManager::initialize (each ShootoutCmd subType
    // has its own ReliableChannel claim). Quickdraw owns the cross-cutting
    // GameEventObserver that routes incoming chain game events into the
    // SupporterReady sub-state when it's active.
    if (chainManager) {
        chainManager->setGameEventObserver(
            [this](uint8_t eventType, const uint8_t* fromMac) {
                if (supporterReadyState != nullptr && currentState != nullptr
                    && currentState->getStateId() == SUPPORTER_READY) {
                    supporterReadyState->onChainGameEventReceived(eventType, fromMac);
                }
            });
    }
    if (symbolWirelessManager) {
        symbolWirelessManager->initialize(wirelessManager, remoteDeviceCoordinator);
        wirelessManager->setEspNowPacketHandler(
            PktType::kSymbolMatchCommand,
            [](const uint8_t* macAddress, const uint8_t* data, const size_t dataLen, void* ctx) {
                static_cast<SymbolWirelessManager*>(ctx)->processSymbolMatchCommand(macAddress, data, dataLen);
            },
            symbolWirelessManager
        );
    }

    // Fan direct-peer transitions from RDC into the game-layer subscribers.
    // ChainManager re-announces and cascades chain-state derived caches;
    // ShootoutManager routes disconnects through onLocalRDCDisconnect to
    // decide whether to broadcast PEER_LOST.
    remoteDeviceCoordinator->setOnDirectPeerChange(
        [this](SerialIdentifier port,
               std::optional<RemoteDeviceCoordinator::Peer> previous,
               std::optional<RemoteDeviceCoordinator::Peer> current) {
            if (chainManager) chainManager->onDirectPeerChange(port, previous, current);
            if (shootoutManager_ && shootoutManager_->active()) {
                shootoutManager_->onDirectPeerChange(port, previous, current);
            }
        });
}

void Quickdraw::onStateLoop(Device *PDN) {
    if (chainManager) chainManager->sync();
    if (quickdrawWirelessManager) quickdrawWirelessManager->sync();
    // Shootout sync runs every tick so MATCH_START / MATCH_RESULT retries
    // and abandon-driven tournament aborts stay within the ~700ms budget
    // regardless of which Quickdraw sub-state is active.
    if (shootoutManager_) shootoutManager_->sync();

    if (chainManager) {
        bool coordNow = chainManager->isCoordinator();
        if (coordNow != lastIsCoordinator_) {
            LOG_W("CHAIN", "isCoordinator %d -> %d",
                  (int)lastIsCoordinator_, (int)coordNow);
            lastIsCoordinator_ = coordNow;
        }
    }

    {
        State* cur = getCurrentState();
        int sid = cur ? cur->getStateId() : -1;
        if (sid != lastLoggedStateId_) {
            LOG_W("QD", "state -> %d (sup=%d inLoop=%d coord=%d)", sid,
                  chainManager ? (int)chainManager->isSupporter() : -1,
                  chainManager ? (int)chainManager->isInStableLoop() : -1,
                  chainManager ? (int)chainManager->isCoordinator() : -1);
            lastLoggedStateId_ = sid;
        }
    }

    if (!statsLogTimer_.isRunning()) {
        statsLogTimer_.setTimer(kStatsLogIntervalMs);
    } else if (statsLogTimer_.expired()) {
        if (chainManager != nullptr) {
            auto c = chainManager->getRetryStats();
            unsigned long cMean = c.ackCount ? (c.ackLatencyMsSum / c.ackCount) : 0;
            // LOG_W (not LOG_I) because firmware builds with CORE_DEBUG_LEVEL=2
            // which strips info-level calls.
            LOG_W("STATS",
                "CDM s=%u r=%u ab=%u ack=%u/%lums",
                (unsigned)c.sends, (unsigned)c.retries, (unsigned)c.abandons,
                (unsigned)c.ackCount, cMean);
        }
        statsLogTimer_.setTimer(kStatsLogIntervalMs);
    }

    StateMachine::onStateLoop(PDN);
}

Quickdraw::~Quickdraw() {
    player = nullptr;
    remoteDeviceCoordinator = nullptr;
    if (quickdrawWirelessManager) {
        quickdrawWirelessManager->clearCallbacks();
    }
    quickdrawWirelessManager = nullptr;
    delete matchManager;
    matchManager = nullptr;
    symbolWirelessManager = nullptr;
    delete chainManager;
    chainManager = nullptr;
    delete shootoutManager_;
    shootoutManager_ = nullptr;
    storageManager = nullptr;
    peerComms = nullptr;
    matches.clear();
}

void Quickdraw::populateStateMap() {

    // Sub-state machines for player registration and handshake
    PlayerRegistrationApp* playerRegistration = new PlayerRegistrationApp(player, wirelessManager, matchManager, remoteDebugManager);
    // Quickdraw gameplay states
    AwakenSequence* awakenSequence = new AwakenSequence(player);
    Idle* idle = new Idle(player, matchManager, remoteDeviceCoordinator, chainManager);

    DuelCountdown* duelCountdown = new DuelCountdown(player, matchManager, remoteDeviceCoordinator, chainManager);
    Duel* duel = new Duel(player, matchManager, remoteDeviceCoordinator, chainManager, shootoutManager_);
    DuelPushed* duelPushed = new DuelPushed(player, matchManager, remoteDeviceCoordinator);
    DuelReceivedResult* duelReceivedResult = new DuelReceivedResult(player, matchManager, remoteDeviceCoordinator);
    DuelResult* duelResult = new DuelResult(player, matchManager, quickdrawWirelessManager, shootoutManager_);
    SupporterReady* supporterReady = new SupporterReady(player, remoteDeviceCoordinator, chainManager);
    this->supporterReadyState = supporterReady;

    Win* win = new Win(player, chainManager, matchManager);
    Lose* lose = new Lose(player, chainManager, matchManager);

    Sleep* sleep = new Sleep(player);
    UploadMatchesState* uploadMatches = new UploadMatchesState(player, wirelessManager, matchManager);

    // Shootout tournament states (auto-triggered by loop closure from Idle).
    ShootoutManager* sht = shootoutManager_;
    auto* shProposal = new ShootoutProposal(sht, chainManager);
    auto* shBracketReveal = new ShootoutBracketReveal(sht, chainManager);
    auto* shSpectator = new ShootoutSpectator(sht);
    auto* shEliminated = new ShootoutEliminated(sht);
    auto* shFinalStandings = new ShootoutFinalStandings(sht, chainManager);
    auto* shAborted = new ShootoutAborted(sht);

    SymbolState* symbol = new SymbolState(player, matchManager, remoteDeviceCoordinator, symbolWirelessManager);
    SymbolMatched* symbolMatched = new SymbolMatched(player, remoteDeviceCoordinator, symbolWirelessManager);

    // --- Transitions from PlayerRegistration app ---
    playerRegistration->addTransition(
        new StateTransition(
            std::bind(&PlayerRegistrationApp::readyForGameplay, playerRegistration),
            awakenSequence));

    // --- Quickdraw gameplay transitions ---
    awakenSequence->addTransition(
        new StateTransition(
            std::bind(&AwakenSequence::transitionToIdle, awakenSequence),
            idle));

    // Auto-trigger Shootout when the chain closes into a loop. Priority over
    // DuelCountdown/SupporterReady: in a closed ring, adjacent H-B pairs
    // otherwise look like a normal duel initiation, and the ring intent
    // (tournament) would be silently demoted to a 1v1 duel.
    {
        ChainManager* cdm = chainManager;
        // Every loop participant self-promotes into ShootoutProposal the
        // moment its roster is stable + closed. Checked before the duel and
        // supporter transitions below, so first-match-wins makes the ring take
        // precedence over a 1v1 without any predicate needing to say so.
        idle->addTransition(
            new StateTransition(
                [cdm]() { return cdm && cdm->isInStableLoop(); },
                shProposal));
    }

    // Duel and supporter entries act only on a settled roster, and only after
    // the shootout edge above has had its chance. During convergence the device
    // holds in Idle and moves once, cleanly, when the topology stops churning.
    {
        ChainManager* cdm = chainManager;
        Idle* idleState = idle;
        idle->addTransition(
            new StateTransition(
                [cdm, idleState]() {
                    return cdm && cdm->isTopologyStable() &&
                           idleState->transitionToDuelCountdown();
                },
                duelCountdown));
        idle->addTransition(
            new StateTransition(
                [cdm, idleState]() {
                    return cdm && cdm->isTopologyStable() &&
                           idleState->transitionToSupporterReady();
                },
                supporterReady));
    }

    {
        ShootoutManager* shMgr = shootoutManager_;
        auto phaseIsAborted = [shMgr]() {
            return shMgr && shMgr->getPhase() == ShootoutManager::Phase::ABORTED;
        };
        idle->addTransition(new StateTransition(phaseIsAborted, shAborted));
        duelCountdown->addTransition(new StateTransition(phaseIsAborted, shAborted));
        duel->addTransition(new StateTransition(phaseIsAborted, shAborted));
        duelPushed->addTransition(new StateTransition(phaseIsAborted, shAborted));
        duelReceivedResult->addTransition(new StateTransition(phaseIsAborted, shAborted));
        duelResult->addTransition(new StateTransition(phaseIsAborted, shAborted));
    }

    supporterReady->addTransition(
        new StateTransition(
            std::bind(&SupporterReady::transitionToIdle, supporterReady),
            idle));

    // --- Duel flow ---
    duelCountdown->addTransition(
        new StateTransition(
            std::bind(&DuelCountdown::shallWeBattle, duelCountdown),
            duel));

    {
        ShootoutManager* shMgr = shootoutManager_;
        duelCountdown->addTransition(
            new StateTransition(
                [duelCountdown, shMgr]() {
                    if (shMgr && shMgr->active()) return false;
                    return duelCountdown->disconnectedBackToIdle();
                },
                idle));
    }

    duel->addTransition(
        new StateTransition(
            std::bind(&Duel::transitionToShootoutSpectator, duel),
            shSpectator));

    duel->addTransition(
        new StateTransition(
            std::bind(&Duel::transitionToShootoutEliminated, duel),
            shEliminated));

    duel->addTransition(
        new StateTransition(
            std::bind(&Duel::transitionToIdle, duel),
            idle));

    duel->addTransition(
        new StateTransition(
            std::bind(&Duel::transitionToDuelReceivedResult, duel),
            duelReceivedResult));

    duel->addTransition(
        new StateTransition(
            std::bind(&Duel::transitionToDuelPushed, duel),
            duelPushed));

    {
        ShootoutManager* shMgr = shootoutManager_;
        duelPushed->addTransition(
            new StateTransition(
                [duelPushed, shMgr]() {
                    if (shMgr && shMgr->active()) return false;
                    return duelPushed->disconnectedBackToIdle();
                },
                idle));
    }

    duelPushed->addTransition(
        new StateTransition(
            std::bind(&DuelPushed::transitionToDuelResult, duelPushed),
            duelResult));

    {
        ShootoutManager* shMgr = shootoutManager_;
        duelReceivedResult->addTransition(
            new StateTransition(
                [duelReceivedResult, shMgr]() {
                    if (shMgr && shMgr->active()) return false;
                    return duelReceivedResult->disconnectedBackToIdle();
                },
                idle));
    }

    duelReceivedResult->addTransition(
        new StateTransition(
            std::bind(&DuelReceivedResult::transitionToDuelResult, duelReceivedResult),
            duelResult));

    // Order matters: the void/abort/idle group must precede win/lose so
    // voided matches never trip them. The two void paths are mutually
    // exclusive — the abort predicate fires only when shootout is active,
    // the Idle predicate only when it isn't.
    duelResult->addTransition(
        new StateTransition(
            std::bind(&DuelResult::transitionToShootoutAbortOnVoid, duelResult),
            shAborted));

    duelResult->addTransition(
        new StateTransition(
            std::bind(&DuelResult::transitionToIdleOnVoid, duelResult),
            idle));

    duelResult->addTransition(
        new StateTransition(
            std::bind(&DuelResult::transitionToWin, duelResult),
            win));

    duelResult->addTransition(
        new StateTransition(
            std::bind(&DuelResult::transitionToLose, duelResult),
            lose));

    duelResult->addTransition(
        new StateTransition(
            std::bind(&DuelResult::transitionToShootoutSpectator, duelResult),
            shSpectator));

    duelResult->addTransition(
        new StateTransition(
            std::bind(&DuelResult::transitionToShootoutEliminated, duelResult),
            shEliminated));

    // --- Post-game flow ---
    win->addTransition(
        new StateTransition(
            std::bind(&Win::resetGame, win),
            uploadMatches));

    lose->addTransition(
        new StateTransition(
            std::bind(&Lose::resetGame, lose),
            uploadMatches));

    uploadMatches->addTransition(
        new StateTransition(
            std::bind(&UploadMatchesState::transitionToSleep, uploadMatches),
            sleep));

    sleep->addTransition(
        new StateTransition(
            std::bind(&Sleep::transitionToAwakenSequence, sleep),
            awakenSequence));

    // --- Shootout transitions ---
    shProposal->addTransition(
        new StateTransition(
            std::bind(&ShootoutProposal::transitionToBracketReveal, shProposal),
            shBracketReveal));
    shProposal->addTransition(
        new StateTransition(
            std::bind(&ShootoutProposal::transitionToAborted, shProposal),
            shAborted));
    shProposal->addTransition(
        new StateTransition(
            std::bind(&ShootoutProposal::transitionToIdle, shProposal),
            idle));

    shBracketReveal->addTransition(
        new StateTransition(
            std::bind(&ShootoutBracketReveal::transitionToDuelCountdown, shBracketReveal),
            duelCountdown));
    shBracketReveal->addTransition(
        new StateTransition(
            std::bind(&ShootoutBracketReveal::transitionToSpectator, shBracketReveal),
            shSpectator));
    shBracketReveal->addTransition(
        new StateTransition(
            std::bind(&ShootoutBracketReveal::transitionToAborted, shBracketReveal),
            shAborted));
    shBracketReveal->addTransition(
        new StateTransition(
            std::bind(&ShootoutBracketReveal::transitionToIdle, shBracketReveal),
            idle));

    shSpectator->addTransition(
        new StateTransition(
            std::bind(&ShootoutSpectator::transitionToDuelCountdown, shSpectator),
            duelCountdown));
    shSpectator->addTransition(
        new StateTransition(
            std::bind(&ShootoutSpectator::transitionToFinalStandings, shSpectator),
            shFinalStandings));
    shSpectator->addTransition(
        new StateTransition(
            std::bind(&ShootoutSpectator::transitionToAborted, shSpectator),
            shAborted));

    shEliminated->addTransition(
        new StateTransition(
            std::bind(&ShootoutEliminated::transitionToFinalStandings, shEliminated),
            shFinalStandings));
    shEliminated->addTransition(
        new StateTransition(
            std::bind(&ShootoutEliminated::transitionToAborted, shEliminated),
            shAborted));

    shAborted->addTransition(
        new StateTransition(
            std::bind(&ShootoutAborted::transitionToIdle, shAborted),
            idle));

    // After TOURNAMENT_END the standings screen returns to Idle, on either a
    // cable unplug or the 10s display timeout, so the device stays playable
    // without a power-cycle. Safe under the peer-graph protocol: a re-proposal
    // only fires from Idle when the ring is still closed AND stable.
    shFinalStandings->addTransition(
        new StateTransition(
            std::bind(&ShootoutFinalStandings::transitionToIdle, shFinalStandings),
            idle));

    // --- Symbol-match transitions ---
    idle->addTransition(
        new StateTransition(
            std::bind(&Idle::transitionToSymbol, idle),
            symbol));

    symbol->addTransition(
        new StateTransition(
            std::bind(&SymbolState::transitionToIdle, symbol),
            idle));

    symbol->addTransition(
        new StateTransition(
            std::bind(&SymbolState::transitionToSymbolMatched, symbol),
            symbolMatched));

    symbolMatched->addTransition(
        new StateTransition(
            std::bind(&SymbolMatched::transitionToSymbol, symbolMatched),
            symbol));

    symbolMatched->addTransition(
        new StateTransition(
            std::bind(&SymbolMatched::transitionToIdle, symbolMatched),
            idle));

    // State map - order matters: first entry is the initial state
    stateMap.push_back(playerRegistration);
    stateMap.push_back(awakenSequence);
    stateMap.push_back(idle);
    stateMap.push_back(supporterReady);
    stateMap.push_back(duelCountdown);
    stateMap.push_back(duel);
    stateMap.push_back(duelPushed);
    stateMap.push_back(duelReceivedResult);
    stateMap.push_back(duelResult);
    stateMap.push_back(win);
    stateMap.push_back(lose);
    stateMap.push_back(uploadMatches);
    stateMap.push_back(sleep);
    stateMap.push_back(shProposal);
    stateMap.push_back(shBracketReveal);
    stateMap.push_back(shSpectator);
    stateMap.push_back(shEliminated);
    stateMap.push_back(shFinalStandings);
    stateMap.push_back(shAborted);
    stateMap.push_back(symbol);
    stateMap.push_back(symbolMatched);
}
