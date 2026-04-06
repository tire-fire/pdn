#include "game/quickdraw-states.hpp"
#include "game/quickdraw.hpp"
#include "game/quickdraw-resources.hpp"
#include "game/jack-roles.hpp"
#include "device/device.hpp"
#include "wireless/team-packet.hpp"
#include <string>

SupporterReady::SupporterReady(Player* player, RemoteDeviceCoordinator* rdc, Quickdraw* quickdraw)
    : State(SUPPORTER_READY), player_(player), rdc_(rdc), quickdraw_(quickdraw) {}

SupporterReady::~SupporterReady() = default;

void SupporterReady::onButtonPressed(void* ctx) {
    auto* state = static_cast<SupporterReady*>(ctx);
    if (!state->duelActive_ || state->hasConfirmed_) return;
    state->hasConfirmed_ = true;
    sendTeamPacket(state->peerComms_, state->championMac_, TeamCommandType::CONFIRM, state->position_);
    state->renderDisplay("Sent!");
}

void SupporterReady::renderDisplay(const char* status) {
    if (!device_) return;
    device_->getDisplay()->invalidateScreen();
    device_->getDisplay()->drawImage(getImageForAllegiance(player_->getAllegiance(), ImageType::IDLE));
    device_->getDisplay()->setGlyphMode(FontMode::TEXT_INVERTED_SMALL)->drawText(championName_[0] ? championName_ : "Support", 68, 20);
    device_->getDisplay()->setGlyphMode(FontMode::TEXT_INVERTED_SMALL)->drawText(status, 68, 40);
    device_->getDisplay()->render();
}

void SupporterReady::onStateMounted(Device* PDN) {
    device_ = PDN;
    peerComms_ = PDN->getPeerComms();
    hasConfirmed_ = false;
    duelActive_ = false;
    transitionToWin_ = false;
    transitionToLose_ = false;
    transitionToIdle_ = false;

    if (quickdraw_) {
        quickdraw_->setGameEventCallback([this](GameEventType evt) {
            handleGameEvent(evt);
        });
    }

    PDN->getPrimaryButton()->setButtonPress(onButtonPressed, this, ButtonInteraction::CLICK);
    PDN->getSecondaryButton()->setButtonPress(onButtonPressed, this, ButtonInteraction::CLICK);

    downstreamInvited_ = false;
    registered_ = false;
    retryTimer_.setTimer(RETRY_MS);

    renderDisplay("Ready");
    timeoutTimer_.setTimer(SUPPORTER_TIMEOUT_MS);
}

void SupporterReady::onStateLoop(Device* PDN) {
    (void)PDN;

    if (timeoutTimer_.expired()) {
        transitionToIdle_ = true;
        return;
    }

    retryTimer_.updateTime();
    bool shouldRetry = retryTimer_.expired();

    // Send/retry REGISTER to champion
    if (!registered_ || shouldRetry) {
        sendTeamPacket(peerComms_, championMac_, TeamCommandType::REGISTER, position_);
        registered_ = true;
    }

    // Forward/retry invite to downstream same-role peer
    SerialIdentifier downstreamJack = supporterJack(player_);
    if (rdc_->getPortStatus(downstreamJack) == PortStatus::CONNECTED &&
        rdc_->getPeerIsHunter(downstreamJack) == player_->isHunter()) {
        if (!downstreamInvited_ || shouldRetry) {
            const uint8_t* downstreamMac = rdc_->getPeerMac(downstreamJack);
            if (downstreamMac && memcmp(downstreamMac, championMac_, 6) != 0) {
                sendRegisterInvite(peerComms_, downstreamMac, position_, championMac_, championName_);
                downstreamInvited_ = true;
            }
        }
    }

    if (shouldRetry) retryTimer_.setTimer(RETRY_MS);

    // Re-invite from a different champion means the chain merged — rejoin under new champion
    if (quickdraw_ && quickdraw_->hasTeamInvite() &&
        memcmp(quickdraw_->getInviteChampionMac(), championMac_, 6) != 0) {
        sendTeamPacket(peerComms_, championMac_, TeamCommandType::DEREGISTER, position_);
        transitionToIdle_ = true;
        return;
    }

    SerialIdentifier championJack = opponentJack(player_);
    if (!transitionToIdle_ && rdc_->getPortStatus(championJack) == PortStatus::DISCONNECTED) {
        sendTeamPacket(peerComms_, championMac_, TeamCommandType::DEREGISTER, position_);
        transitionToIdle_ = true;
    }
}

void SupporterReady::handleGameEvent(GameEventType evt) {
    switch (evt) {
        case GameEventType::COUNTDOWN:
            duelActive_ = true;
            if (!hasConfirmed_) renderDisplay("Press!");
            break;
        case GameEventType::DRAW:
            duelActive_ = true;
            if (!hasConfirmed_) renderDisplay("Press!");
            break;
        case GameEventType::WIN:
            renderDisplay("WIN!");
            transitionToWin_ = true;
            break;
        case GameEventType::LOSS:
            renderDisplay("LOSS");
            transitionToLose_ = true;
            break;
    }
}

void SupporterReady::onStateDismounted(Device* PDN) {
    if (quickdraw_) quickdraw_->clearGameEventCallback();
    PDN->getPrimaryButton()->removeButtonCallbacks();
    PDN->getSecondaryButton()->removeButtonCallbacks();
    timeoutTimer_.invalidate();
    PDN->getPeerComms()->removePeer(const_cast<uint8_t*>(championMac_));
    device_ = nullptr;
}

bool SupporterReady::transitionToWin() { return transitionToWin_; }
bool SupporterReady::transitionToLose() { return transitionToLose_; }
bool SupporterReady::transitionToIdle() { return transitionToIdle_; }
