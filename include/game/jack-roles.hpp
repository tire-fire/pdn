#pragma once

#include "game/player.hpp"
#include "device/drivers/serial-wrapper.hpp"

// Jack semantics:
//   Hunter: OUTPUT → opponent, INPUT ← supporter
//   Bounty: INPUT ← opponent, OUTPUT → supporter
//
// Behaviors:
//   sendInvites   = hasSameRolePeer on supporterJack
//   acceptInvites = !sendInvites (no same-role peer on supporterJack)
//   initMatch     = isHunter AND opponentJack connected
//   forwardInvite = in SupporterReady AND hasSameRolePeer on supporterJack
inline SerialIdentifier opponentJack(const Player* p) {
    return p->isHunter() ? SerialIdentifier::OUTPUT_JACK : SerialIdentifier::INPUT_JACK;
}

inline SerialIdentifier supporterJack(const Player* p) {
    return p->isHunter() ? SerialIdentifier::INPUT_JACK : SerialIdentifier::OUTPUT_JACK;
}
