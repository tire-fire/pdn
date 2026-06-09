#include "game/quickdraw-states.hpp"
#include "game/quickdraw-resources.hpp"
#include "game/chain-duel-manager.hpp"
#include "game/match-manager.hpp"
#include "device/device.hpp"
#include <cstdio>

DuelOutcome::DuelOutcome(Player *player, ChainDuelManager* chainDuelManager, MatchManager* matchManager, bool won)
    : State(won ? WIN : LOSE), player(player), chainDuelManager(chainDuelManager),
      matchManager(matchManager), won_(won) {}

DuelOutcome::~DuelOutcome() {
    player = nullptr;
    matchManager = nullptr;
}

void DuelOutcome::onStateMounted(Device *PDN) {
    // Propagate the outcome to the supporter chain (no-op for non-champion).
    chainDuelManager->sendGameEventToSupporters(won_ ? ChainGameEventType::WIN : ChainGameEventType::LOSS);

    PDN->getHaptics()->setIntensity(VIBRATION_OFF);

    PDN->getDisplay()->invalidateScreen()
        ->drawImage(getImageForAllegiance(player->getAllegiance(),
                                          won_ ? ImageType::WIN : ImageType::LOSE));

    if (matchManager != nullptr) {
        const LastMatchDisplay& d = matchManager->getLastMatchDisplay();
        if (d.hasData) {
            PDN->getDisplay()->drawImage(Image(image_stats_card, 60, 64, 0, 0));
            char myLine[16];
            char boostLine[16];
            char oppLine[16];
            snprintf(myLine, sizeof(myLine), "You:%4lu", d.myTimeMs);
            snprintf(oppLine, sizeof(oppLine), "Opp:%4lu", d.opponentTimeMs);
            PDN->getDisplay()->setGlyphMode(FontMode::TEXT_INVERTED_SMALL)
                ->drawText(myLine, 2, 12);
            if (d.boostMs > 0) {
                unsigned long boosts = d.boostMs / ChainDuelManager::BOOST_PER_SUPPORTER_MS;
                snprintf(boostLine, sizeof(boostLine), "Boost: %lu", boosts);
                PDN->getDisplay()->setGlyphMode(FontMode::TEXT_INVERTED_SMALL)
                    ->drawText(boostLine, 2, 28);
            }
            PDN->getDisplay()->setGlyphMode(FontMode::TEXT_INVERTED_SMALL)
                ->drawText(oppLine, 2, 44);
        }
    }

    PDN->getDisplay()->render();

    outcomeTimer.setTimer(8000);

    AnimationConfig config;
    config.type = won_ ? (player->isHunter() ? AnimationType::HUNTER_WIN : AnimationType::BOUNTY_WIN)
                       : AnimationType::LOSE;
    config.loop = true;
    config.speed = 16;
    config.initialState = LEDState();
    config.loopDelayMs = 0;

    PDN->getLightManager()->startAnimation(config);
}

void DuelOutcome::onStateLoop(Device *PDN) {
    outcomeTimer.updateTime();
}

void DuelOutcome::onStateDismounted(Device *PDN) {
    outcomeTimer.invalidate();
    PDN->getHaptics()->setIntensity(VIBRATION_OFF);
}

bool DuelOutcome::resetGame() {
    return outcomeTimer.expired();
}

bool DuelOutcome::isTerminalState() {
    return true;
}
