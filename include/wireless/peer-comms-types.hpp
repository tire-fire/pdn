#pragma once

#include <cstddef>
#include <cstdint>

//PktType determines which callback will handle the packet on the receiving end
enum class PktType : uint8_t
{
    kPlayerInfoBroadcast = 0,
    kQuickdrawCommand = 1,
    kDebugPacket = 2,
    kHandshakeCommand = 3,
    kChainGameEvent = 5,
    kChainConfirm = 6,
    kRoleAnnounce = 7,
    kShootoutCommand = 8,
    kSymbolMatchCommand = 9,
    kAck = 10,
    kNumPacketTypes //Not a real packet type, DO NOT USE
};

// Max length of a player name on the wire (no null terminator).
inline constexpr size_t kNameLength = 12;

struct AckPayload
{
    uint8_t originalType;
    uint8_t subType;
    uint8_t seqId;
} __attribute__((packed));

struct DataPktHdr
{
    //Total packet length including header
    uint8_t pktLen;
    PktType packetType;
    uint8_t numPktsInCluster;
    uint8_t idxInCluster;
} __attribute__((packed));

struct ChainConfirmPayload
{
    uint8_t originatorMac[6];
    uint8_t seqId;
} __attribute__((packed));

struct RoleAnnouncePayload
{
    uint8_t role;               // 1 = hunter, 0 = target/bounty
    uint8_t championMac[6];
    uint8_t seqId;
} __attribute__((packed));

enum class ShootoutCmd : uint8_t
{
    CONFIRM = 0,
    MATCH_START = 2,
    MATCH_RESULT = 3,
    TOURNAMENT_END = 4,
    PEER_LOST = 5,
    ABORT = 6,
    BRACKET_ENTRY = 7,
};

// Wire format shared between ChainManager send sites and ChainGameEvent
// receivers; must stay packed. Holds enough room for the largest game-event
// payload currently in flight; fields beyond event_type and seqId are
// populated only by event types that need them.
struct ChainGameEventPayload
{
    uint8_t event_type; // ChainGameEventType
    uint8_t seqId;      // 0 = no-ack/no-retry; nonzero = reliable
    uint8_t payload[14];
} __attribute__((packed));

struct ShootoutConfirmPayload {
    uint8_t cmd;
    uint8_t seqId;
    uint8_t mac[6];
    char    name[kNameLength];
} __attribute__((packed));

struct ShootoutMatchStartPayload {
    uint8_t cmd;
    uint8_t seqId;
    uint8_t duelistA[6];
    uint8_t duelistB[6];
    uint8_t matchIndex;
} __attribute__((packed));

struct ShootoutMatchResultPayload {
    uint8_t cmd;
    uint8_t seqId;
    uint8_t winner[6];
    uint8_t loser[6];
    uint8_t matchIndex;
} __attribute__((packed));

struct ShootoutBracketEntryPayload {
    uint8_t cmd;          // ShootoutCmd::BRACKET_ENTRY
    uint8_t seqId;
    uint8_t batchId;
    uint8_t slot;
    uint8_t totalSlots;
    uint8_t mac[6];
} __attribute__((packed));

struct ShootoutTournamentEndPayload {
    uint8_t cmd;
    uint8_t seqId;
    uint8_t winner[6];
} __attribute__((packed));

struct ShootoutPeerLostPayload {
    uint8_t cmd;
    uint8_t seqId;
    uint8_t mac[6];
} __attribute__((packed));

struct ShootoutAbortPayload {
    uint8_t cmd;
    uint8_t seqId;
} __attribute__((packed));

