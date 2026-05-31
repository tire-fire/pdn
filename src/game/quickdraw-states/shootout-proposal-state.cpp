#include "game/quickdraw-states.hpp"
#include "device/device.hpp"

ShootoutProposal::ShootoutProposal(ShootoutManager* shootout, ChainManager* chainManager)
    : State(SHOOTOUT_PROPOSAL), shootout_(shootout), chainManager_(chainManager) {}

void ShootoutProposal::onStateMounted(Device *PDN) {
    shootout_->startProposal();
    auto* d = PDN->getDisplay();
    d->invalidateScreen()->setGlyphMode(FontMode::TEXT_INVERTED_LARGE);
    d->drawCenteredText("SHOOTOUT", 15);
    d->setGlyphMode(FontMode::TEXT_INVERTED_SMALL);
    d->drawCenteredText("press to", 40);
    d->drawCenteredText("confirm", 55);
    d->render();

    parameterizedCallbackFunction confirmCb = [](void *ctx) {
        static_cast<ShootoutProposal*>(ctx)->shootout_->confirmLocal();
    };
    PDN->getPrimaryButton()->setButtonPress(confirmCb, this, ButtonInteraction::CLICK);
    PDN->getSecondaryButton()->setButtonPress(confirmCb, this, ButtonInteraction::CLICK);
}

void ShootoutProposal::onStateLoop(Device *PDN) {
    auto p = shootout_->getPhase();
    if (p == ShootoutManager::Phase::BRACKET_REVEAL) shouldGoToReveal_ = true;
    if (p == ShootoutManager::Phase::ABORTED) shouldGoToAborted_ = true;
    if (p == ShootoutManager::Phase::IDLE) shouldGoToIdle_ = true;
    // Abort once the roster has SETTLED into a non-loop. The stability term is
    // what makes us hold during convergence: !isInStableLoop() alone is also
    // true mid-churn (unstable), which would abort on a transient blip. Gating
    // on isRosterStable() means we only act on a settled topology, so a quick
    // unplug→replug that never settles as a chain keeps us in the proposal.
    // abortTournament sets phase=ABORTED; the phase check above then routes
    // every member through the Aborted screen back to idle.
    bool ringSettledOpen = chainManager_ &&
                           chainManager_->isRosterStable() &&
                           !chainManager_->isInStableLoop();
    if (loopBreakDebounce_.heldFor(ringSettledOpen, kLoopBreakDebounceMs)) {
        shootout_->abortTournament();
    }
}

void ShootoutProposal::onStateDismounted(Device *PDN) {
    PDN->getPrimaryButton()->removeButtonCallbacks();
    PDN->getSecondaryButton()->removeButtonCallbacks();
    shouldGoToReveal_ = false;
    shouldGoToIdle_ = false;
    shouldGoToAborted_ = false;
    loopBreakDebounce_.reset();
}

bool ShootoutProposal::transitionToBracketReveal() { return shouldGoToReveal_; }
bool ShootoutProposal::transitionToIdle() { return shouldGoToIdle_; }
bool ShootoutProposal::transitionToAborted() { return shouldGoToAborted_; }
