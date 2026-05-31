#include "game/quickdraw-states.hpp"
#include "device/device.hpp"
#include <cstdio>
#include <cstring>

ShootoutFinalStandings::ShootoutFinalStandings(ShootoutManager* shootout, ChainManager* chainManager)
    : State(SHOOTOUT_FINAL_STANDINGS), shootout_(shootout), chainManager_(chainManager) {}

void ShootoutFinalStandings::onStateMounted(Device *PDN) {
    PDN->getLightManager()->stopAnimation();
    displayTimer_.setTimer(STANDINGS_DISPLAY_MS);
    auto winner = shootout_->getTournamentWinner();
    const uint8_t* selfMac = PDN->getWirelessManager()->getMacAddress();
    bool iWon = selfMac && memcmp(winner.data(), selfMac, 6) == 0;
    std::string winnerName = shootout_->getNameForMac(winner.data());
    const char* header = iWon ? "VICTORY" : "DEFEAT";
    auto* d = PDN->getDisplay();
    d->invalidateScreen()->setGlyphMode(FontMode::TEXT_INVERTED_LARGE);
    d->drawCenteredText(header, 20);
    d->setGlyphMode(FontMode::TEXT_INVERTED_SMALL);
    d->drawCenteredText("winner:", 40);
    d->drawCenteredText(winnerName.c_str(), 55);
    d->render();
}

void ShootoutFinalStandings::onStateLoop(Device *PDN) {
    // Leave the standings screen back to Idle on either trigger: the player
    // unplugs a cable (ring opens), or the display timeout elapses. Returning
    // to Idle (not Sleep) keeps the device playable without a power-cycle.
    // No phantom re-proposal results: Idle→ShootoutProposal is gated on
    // isTopologyStable() && isInLoop(), so a device whose ring has opened sees
    // no loop and starts no proposal.
    auto* rdc = PDN ? PDN->getRemoteDeviceCoordinator() : nullptr;
    if (rdc == nullptr) return;
    bool ringStillClosed = (rdc->getPeerMac(SerialIdentifier::OUTPUT_JACK) != nullptr) &&
                           (rdc->getPeerMac(SerialIdentifier::INPUT_JACK) != nullptr);
    if (!ringStillClosed || displayTimer_.expired()) {
        shouldGoToIdle_ = true;
    }
}

void ShootoutFinalStandings::onStateDismounted(Device *PDN) {
    // Reset Shootout state so the next loop closure triggers a fresh
    // proposal. Without this, phase_ stays ENDED and shootoutManager->active()
    // returns true, blocking the Idle→ShootoutProposal transition.
    if (shootout_) shootout_->resetToIdle();
    displayTimer_.invalidate();
    shouldGoToIdle_ = false;
}

bool ShootoutFinalStandings::isTerminalState() { return false; }

bool ShootoutFinalStandings::transitionToIdle() { return shouldGoToIdle_; }
