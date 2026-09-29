#include "game/quickdraw-states.hpp"
#include "device/device.hpp"

ShootoutBracketReveal::ShootoutBracketReveal(const GameContext& ctx)
    : TypedState<PDN>(SHOOTOUT_BRACKET_REVEAL)
    , shootoutManager(ctx.shootoutManager) {}

void ShootoutBracketReveal::onStateMounted(PDN* pdn) {
    // Clear stale button callbacks left by ShootoutProposal.
    pdn->getPrimaryButton()->removeButtonCallbacks();
    pdn->getSecondaryButton()->removeButtonCallbacks();
    auto* d = pdn->getDisplay();
    d->invalidateScreen()->setGlyphMode(FontMode::TEXT_INVERTED_LARGE);
    d->drawCenteredText("BRACKET", 20);
    d->setGlyphMode(FontMode::TEXT_INVERTED_SMALL);
    d->drawCenteredText("ready...", 50);
    d->render();
}

void ShootoutBracketReveal::onStateLoop(PDN* pdn) {
    ShootoutManager::Phase p = shootoutManager->getPhase();
    if (p == ShootoutManager::Phase::MATCH_IN_PROGRESS) {
        if (shootoutManager->isLocalDuelist()) {
            shouldGoToDuelCountdown_ = true;
        } else {
            shouldGoToSpectator_ = true;
        }
    }
    // A device that lost every MATCH_START never leaves for a bout, and the ending
    // repeats until one copy reaches it — so the tournament can finish while this
    // screen is still showing the bracket it joined.
    if (p == ShootoutManager::Phase::ENDED) shouldGoToFinalStandings = true;
}

void ShootoutBracketReveal::onStateDismounted(PDN* pdn) {
    shouldGoToDuelCountdown_ = false;
    shouldGoToSpectator_ = false;
    shouldGoToFinalStandings = false;
}

bool ShootoutBracketReveal::transitionToDuelCountdown() { return shouldGoToDuelCountdown_; }
bool ShootoutBracketReveal::transitionToSpectator() { return shouldGoToSpectator_; }
bool ShootoutBracketReveal::transitionToFinalStandings() { return shouldGoToFinalStandings; }
