
#include "game/quickdraw-states.hpp"
#include "game/match-manager.hpp"
#include "device/drivers/logger.hpp"
#include "device/device.hpp"

#define DUEL_RESULT_RECEIVED_TAG "DUEL_RESULT_RECEIVED"

DuelReceivedResult::DuelReceivedResult(Player* player, MatchManager* matchManager, RemoteDeviceCoordinator* remoteDeviceCoordinator) : ConnectState(remoteDeviceCoordinator, DUEL_RECEIVED_RESULT) {
    this->player = player;
    this->matchManager = matchManager;
}

DuelReceivedResult::~DuelReceivedResult() {
    LOG_I(DUEL_RESULT_RECEIVED_TAG, "Duel result received state destroyed");
    this->player = nullptr;
    this->matchManager = nullptr;
}

void DuelReceivedResult::onStateMounted(Device *PDN) {
    LOG_I(DUEL_RESULT_RECEIVED_TAG, "Duel result received state mounted");

    auto duelButtonPush = matchManager->getDuelButtonPush();
    PDN->getPrimaryButton()->setButtonPress(duelButtonPush, matchManager, ButtonInteraction::CLICK);
    PDN->getSecondaryButton()->setButtonPress(duelButtonPush, matchManager, ButtonInteraction::CLICK);

    buttonPushGraceTimer.setTimer(BUTTON_PUSH_GRACE_PERIOD);
}

void DuelReceivedResult::onStateLoop(Device *PDN) {
    if(matchManager->getHasPressedButton()) {
        PDN->getHaptics()->setIntensity(0);
    }

    buttonPushGraceTimer.updateTime();

    // execDrivers() runs the button handler before this loop, so a press can
    // land the same iteration the grace expires. It must take the press path,
    // not be reported as a no-show: sendNeverPressed sets gracePeriodExpiredNoResult,
    // which flips didWin() to a loss regardless of the recorded reaction time.
    // Mirrors DuelPushed's !matchResultsAreIn() void guard.
    if(buttonPushGraceTimer.expired() && !neverPressedSent_ && !matchManager->getHasPressedButton()) {
        LOG_I(DUEL_RESULT_RECEIVED_TAG, "Button push grace period expired");

        unsigned long pityTime = SimpleTimer::getPlatformClock()->milliseconds() - matchManager->getDuelLocalStartTime();

        matchManager->sendNeverPressed(pityTime);
        neverPressedSent_ = true;
    }
}

void DuelReceivedResult::onStateDismounted(Device *PDN) {
    LOG_I(DUEL_RESULT_RECEIVED_TAG, "Duel result received state dismounted");

    if (!isConnected()) {
        matchManager->clearCurrentMatch();
    }

    neverPressedSent_ = false;
    PDN->getPrimaryButton()->removeButtonCallbacks();
    PDN->getSecondaryButton()->removeButtonCallbacks();
    buttonPushGraceTimer.invalidate();
}

bool DuelReceivedResult::transitionToDuelResult() {
    return matchManager->matchResultsAreIn();
}

bool DuelReceivedResult::disconnectedBackToIdle() {
    return isPersistentlyDisconnected();
}

bool DuelReceivedResult::isPrimaryRequired() {
    return matchManager->localIsHunterForMatch();
}

bool DuelReceivedResult::isAuxRequired() {
    return !matchManager->localIsHunterForMatch();
}