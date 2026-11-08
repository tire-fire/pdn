#include "game/quickdraw-states.hpp"
#include "game/quickdraw-resources.hpp"
#include "device/animation/hunter-win-animation.hpp"
#include "device/animation/bounty-win-animation.hpp"
#include "game/posse-manager.hpp"
#include "game/match-manager.hpp"
#include "device/device.hpp"
#include <cstdio>

Win::Win(const GameContext& ctx)
    : TypedState<PDN>(WIN)
    , player(ctx.player)
    , posseManager(ctx.posseManager)
    , matchManager(ctx.matchManager) {
}

Win::~Win() {
    player = nullptr;
    matchManager = nullptr;
}

void Win::onStateMounted(PDN* pdn) {
    // Propagate the outcome to the supporter chain (no-op for non-challenger).
    posseManager->sendGameEventToSupporters(ChainGameEventType::WIN);

    pdn->getHaptics()->setIntensity(VIBRATION_OFF);

    pdn->getDisplay()->invalidateScreen()
        ->drawImage(getImageForAllegiance(player->getAllegiance(), ImageType::WIN));

    if (matchManager != nullptr) {
        const LastMatchDisplay& d = matchManager->getLastMatchDisplay();
        if (d.hasData) {
            pdn->getDisplay()->drawImage(Image(image_stats_card, 60, 64, 0, 0));
            char myLine[28];
            char oppLine[28];
            snprintf(myLine, sizeof(myLine), "You:%4ld", d.myTimeMs);
            snprintf(oppLine, sizeof(oppLine), "Opp:%4ld", d.opponentTimeMs);
            pdn->getDisplay()->setGlyphMode(FontMode::TEXT_INVERTED_SMALL)
                ->drawText(myLine, 2, 12);
            const bool hasBoost = d.boostMs > 0;
            const bool hasPenalty = d.penaltyMs > 0;
            if (hasBoost) {
                char boostLine[28];
                unsigned long boosts = d.boostMs / PosseManager::BOOST_PER_SUPPORTER_MS;
                snprintf(boostLine, sizeof(boostLine), "Boost: %lu", boosts);
                pdn->getDisplay()->drawText(boostLine, 2, hasPenalty ? 24 : 28);
            }
            if (hasPenalty) {
                char penaltyLine[28];
                snprintf(penaltyLine, sizeof(penaltyLine), "Pen:+%lu", d.penaltyMs);
                pdn->getDisplay()->drawText(penaltyLine, 2, hasBoost ? 36 : 28);
            }
            pdn->getDisplay()->drawText(oppLine, 2, (hasBoost && hasPenalty) ? 48 : 44);
        }
    }

    pdn->getDisplay()->render();

    winTimer.setTimer(8000);

    AnimationBase* animation = player->isHunter()
        ? (AnimationBase*)new HunterWinAnimation()
        : (AnimationBase*)new BountyWinAnimation();
    AnimationConfig config;
    config.loop = true;
    config.speed = 16;
    config.initialState = LEDState();
    config.loopDelayMs = 0;

    pdn->getLightManager()->startAnimation(animation, config);
}

void Win::onStateLoop(PDN* pdn) {
    winTimer.updateTime();
    if(winTimer.expired()) {
        reset = true;
    }
}

void Win::onStateDismounted(PDN* pdn) {
    winTimer.invalidate();
    reset = false;
    pdn->getHaptics()->setIntensity(VIBRATION_OFF);
}

bool Win::resetGame() {
    return reset;
}

bool Win::isTerminalState() {
    return true;
}
