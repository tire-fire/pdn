#include "game/quickdraw-states.hpp"
#include "device/device.hpp"

ShootoutEliminated::ShootoutEliminated(const GameContext& ctx)
    : TypedState<PDN>(SHOOTOUT_ELIMINATED) {}

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


