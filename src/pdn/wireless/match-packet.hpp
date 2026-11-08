#pragma once

#include <cstdint>

#include "id-generator.hpp"

struct MatchPacket {
    char matchId[IdGenerator::UUID_BUFFER_SIZE];
    char playerId[5];
    uint8_t isHunter;
    long playerDrawTime;
    unsigned long rawDrawTime;
    int command;
    uint8_t seqId;
} __attribute__((packed));

namespace MatchCmd {
constexpr int SEND_MATCH_ID = 6;
constexpr int MATCH_ID_ACK = 7;
constexpr int MATCH_ROLE_MISMATCH = 8;
constexpr int DRAW_RESULT = 9;
constexpr int NEVER_PRESSED = 10;
}

struct MatchCommand {
    const uint8_t* wifiMacAddr;
    int command;
    char matchId[IdGenerator::UUID_BUFFER_SIZE];
    char playerId[5];
    bool isHunter;
    long playerDrawTime;
    unsigned long rawDrawTime;

    /** Decoded match frame received from macAddress. */
    MatchCommand(const uint8_t* macAddress, int command, const char* matchId, const char* playerId,
                 long playerDrawTime, bool isHunter, unsigned long rawDrawTime)
        : wifiMacAddr(macAddress)
        , command(command)
        , isHunter(isHunter)
        , playerDrawTime(playerDrawTime)
        , rawDrawTime(rawDrawTime) {
        IdGenerator::copyId(this->matchId, matchId);
        IdGenerator::copyId(this->playerId, playerId);
    }

    /** Convenience overload for unboosted or non-draw commands where raw time equals effective
     * time. */
    MatchCommand(const uint8_t* macAddress, int command, const char* matchId, const char* playerId,
                 long playerDrawTime, bool isHunter)
        : MatchCommand(macAddress, command, matchId, playerId, playerDrawTime, isHunter,
                       static_cast<unsigned long>(playerDrawTime > 0 ? playerDrawTime : 0)) {}
};
