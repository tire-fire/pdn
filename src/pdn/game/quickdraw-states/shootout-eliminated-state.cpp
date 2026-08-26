#include "game/quickdraw-states.hpp"
#include "device/device.hpp"

ShootoutEliminated::ShootoutEliminated(const GameContext& ctx)
    : TypedState<PDN>(SHOOTOUT_ELIMINATED)
    , ShootoutAwareState(ctx.shootoutManager, ctx.chainDuelManager)
    , shootout_(ctx.shootoutManager) {}

void ShootoutEliminated::onStateMounted(PDN* pdn) {
    pdn->getPrimaryButton()->removeButtonCallbacks();
    pdn->getSecondaryButton()->removeButtonCallbacks();
    pdn->getLightManager()->stopAnimation();
    auto* d = pdn->getDisplay();
    d->invalidateScreen()->setGlyphMode(FontMode::TEXT_INVERTED_LARGE);
    d->drawCenteredText("OUT", 20);
    d->setGlyphMode(FontMode::TEXT_INVERTED_SMALL);
    d->drawCenteredText("spectating", 50);
    d->render();
}

void ShootoutEliminated::onStateLoop(PDN* pdn) {
    // An eliminated player waits here until the tournament ends, so the ring
    // breaking under it has to be able to end it too.
    tickAbortGuard();
    auto p = shootout_->getPhase();
    if (p == ShootoutManager::Phase::ENDED) shouldGoToFinalStandings_ = true;
}

void ShootoutEliminated::onStateDismounted(PDN* pdn) {
    resetAbortGuard();
    shouldGoToFinalStandings_ = false;
}

bool ShootoutEliminated::transitionToFinalStandings() { return shouldGoToFinalStandings_; }
