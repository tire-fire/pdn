#include "game/quickdraw-states.hpp"
#include "device/device.hpp"

ShootoutAborted::ShootoutAborted(const GameContext& ctx)
    : TypedState<PDN>(SHOOTOUT_ABORTED)
    , shootout_(ctx.shootoutManager) {}

void ShootoutAborted::onStateMounted(PDN* pdn) {
    pdn->getLightManager()->stopAnimation();
    displayTimer_.setTimer(ABORTED_DISPLAY_MS);
    auto* d = pdn->getDisplay();
    d->invalidateScreen()->setGlyphMode(FontMode::TEXT_INVERTED_LARGE);
    d->drawCenteredText("ABORTED", 30);
    d->render();
}

void ShootoutAborted::onStateLoop(PDN* pdn) {
    if (displayTimer_.expired()) {
        shouldGoToIdle_ = true;
    }
}

void ShootoutAborted::onStateDismounted(PDN* pdn) {
    // Only if the tournament is still the aborted one. A bracket naming this device
    // can land inside ABORTED_DISPLAY_MS, while this screen is up — its head drew just after
    // our bound fired — and onBracketReceived adopts it rather than leaving the head to
    // abandon on us. Resetting unconditionally would throw that away.
    if (shootout_ && shootout_->getPhase() == ShootoutManager::Phase::ABORTED)
        shootout_->resetToIdle();
    displayTimer_.invalidate();
    shouldGoToIdle_ = false;
}

bool ShootoutAborted::transitionToIdle() { return shouldGoToIdle_; }
