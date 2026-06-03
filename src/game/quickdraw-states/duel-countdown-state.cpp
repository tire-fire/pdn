#include "device/drivers/serial-wrapper.hpp"
#include "game/quickdraw-states.hpp"
#include "game/quickdraw-resources.hpp"
#include "game/chain-manager.hpp"
#include "device/device.hpp"
#include "device/drivers/logger.hpp"

DuelCountdown::DuelCountdown(Player* player, MatchManager* matchManager, RemoteDeviceCoordinator* remoteDeviceCoordinator, ChainManager* chainManager) : ConnectState(remoteDeviceCoordinator, DUEL_COUNTDOWN) {
    this->player = player;
    this->matchManager = matchManager;
    this->chainManager = chainManager;
}

DuelCountdown::~DuelCountdown() {
    player = nullptr;
    matchManager = nullptr;
}


void DuelCountdown::onStateMounted(Device *PDN) {
    // If this device is a champion, tell its supporter chain that the
    // duel is starting so they can arm their confirmation window.
    chainManager->sendGameEventToSupporters(ChainGameEventType::COUNTDOWN);

    PDN->getDisplay()->
    invalidateScreen()->
    drawImage(getImageForAllegiance(player->getAllegiance(), getImageIdForStep(countdownQueue[currentStepIndex].step)))->
    render();

    PDN->getLightManager()->startAnimation(countdownQueue[currentStepIndex].animationConfig);

    countdownTimer.setTimer(countdownQueue[currentStepIndex].countdownTimer);
    currentStepIndex++;

    PDN->getPrimaryButton()->setButtonPress(
        matchManager->getButtonMasher(),
        matchManager, ButtonInteraction::CLICK);

    PDN->getSecondaryButton()->setButtonPress(
        matchManager->getButtonMasher(),
        matchManager);

    PDN->getHaptics()->setIntensity(HAPTIC_INTENSITY);
    hapticTimer.setTimer(HAPTIC_DURATION);
}


void DuelCountdown::onStateLoop(Device *PDN) {
    countdownTimer.updateTime();
    hapticTimer.updateTime();

    if (hapticTimer.expired()) {
        PDN->getHaptics()->setIntensity(0);
    }

    if (countdownTimer.expired()) {
        PDN->getHaptics()->setIntensity(HAPTIC_INTENSITY);
        hapticTimer.setTimer(HAPTIC_DURATION);
        if(countdownQueue[currentStepIndex].step == CountdownStep::BATTLE) {
            doBattle = true;
        } else {
            PDN->getDisplay()->
            invalidateScreen()->
            drawImage(getImageForAllegiance(player->getAllegiance(), getImageIdForStep(countdownQueue[currentStepIndex].step)))->
            render();

            PDN->getLightManager()->startAnimation(countdownQueue[currentStepIndex].animationConfig);

            countdownTimer.setTimer(countdownQueue[currentStepIndex].countdownTimer);
            currentStepIndex++;
        }
    }
}

ImageType DuelCountdown::getImageIdForStep(CountdownStep step) {
    switch(step) {
        case CountdownStep::THREE:
            return ImageType::COUNTDOWN_THREE;
        case CountdownStep::TWO:
            return ImageType::COUNTDOWN_TWO;
        default:
            return ImageType::COUNTDOWN_ONE;
    }
}


void DuelCountdown::onStateDismounted(Device *PDN) {
    if (!doBattle) {
        matchManager->clearCurrentMatch();
        // Countdown aborted (opponent unplugged). Tell supporters to disarm
        // so they don't stay stuck on "PRESS".
        if (chainManager != nullptr) {
            chainManager->sendGameEventToSupporters(ChainGameEventType::DRAW);
        }
    }

    doBattle = false;
    currentStepIndex = 0;
    countdownTimer.invalidate();
    hapticTimer.invalidate();
    // The countdown pulses the haptic for HAPTIC_DURATION on each step and
    // relies on onStateLoop to clear it when the pulse timer expires. If we
    // leave mid-pulse (loop-close yield, abort, disconnect), that clear never
    // runs and the motor latches on. Force it off here.
    PDN->getHaptics()->setIntensity(0);
    PDN->getPrimaryButton()->removeButtonCallbacks();
    PDN->getSecondaryButton()->removeButtonCallbacks();
}

bool DuelCountdown::shallWeBattle() {
    return doBattle;
}

bool DuelCountdown::disconnectedBackToIdle() {
    return isPersistentlyDisconnected();
}

bool DuelCountdown::isPrimaryRequired() {
    return matchManager->localIsHunterForMatch();
}

bool DuelCountdown::isAuxRequired() {
    return !matchManager->localIsHunterForMatch();
}