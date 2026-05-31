#include "game/quickdraw-states.hpp"
#include "device/device.hpp"

ShootoutBracketReveal::ShootoutBracketReveal(ShootoutManager* shootout, ChainManager* chainManager)
    : State(SHOOTOUT_BRACKET_REVEAL), shootout_(shootout), chainManager_(chainManager) {}

void ShootoutBracketReveal::onStateMounted(Device *PDN) {
    // Clear stale button callbacks left by ShootoutProposal.
    PDN->getPrimaryButton()->removeButtonCallbacks();
    PDN->getSecondaryButton()->removeButtonCallbacks();
    auto* d = PDN->getDisplay();
    d->invalidateScreen()->setGlyphMode(FontMode::TEXT_INVERTED_LARGE);
    d->drawCenteredText("BRACKET", 20);
    d->setGlyphMode(FontMode::TEXT_INVERTED_SMALL);
    d->drawCenteredText("ready...", 50);
    d->render();
}

void ShootoutBracketReveal::onStateLoop(Device *PDN) {
    auto p = shootout_->getPhase();
    if (p == ShootoutManager::Phase::MATCH_IN_PROGRESS) {
        if (shootout_->isLocalDuelist()) {
            shouldGoToDuelCountdown_ = true;
        } else {
            shouldGoToSpectator_ = true;
        }
    }
    if (p == ShootoutManager::Phase::ABORTED) shouldGoToAborted_ = true;
    // Abort only once the roster has SETTLED into a non-loop (see
    // ShootoutProposal for why the isRosterStable term is required, not just
    // !isInStableLoop). Routes through ShootoutAborted via phase=ABORTED.
    bool ringSettledOpen = chainManager_ &&
                           chainManager_->isRosterStable() &&
                           !chainManager_->isInStableLoop();
    if (loopBreakDebounce_.heldFor(ringSettledOpen, kLoopBreakDebounceMs)) {
        shootout_->abortTournament();
    }
}

void ShootoutBracketReveal::onStateDismounted(Device *PDN) {
    shouldGoToDuelCountdown_ = false;
    shouldGoToSpectator_ = false;
    shouldGoToAborted_ = false;
    shouldGoToIdle_ = false;
    loopBreakDebounce_.reset();
}

bool ShootoutBracketReveal::transitionToDuelCountdown() { return shouldGoToDuelCountdown_; }
bool ShootoutBracketReveal::transitionToSpectator() { return shouldGoToSpectator_; }
bool ShootoutBracketReveal::transitionToAborted() { return shouldGoToAborted_; }
bool ShootoutBracketReveal::transitionToIdle() { return shouldGoToIdle_; }
