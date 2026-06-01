#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>
#include "game/player.hpp"
#include "game/chain-manager.hpp"
#include "device/remote-device-coordinator.hpp"
#include "device/wireless-manager.hpp"
#include "utils/simple-timer.hpp"
#include "wireless/resender.hpp"
#include "wireless/wireless-transport.hpp"

class MatchManager;
class WirelessTransport;

// Prefix for shootout-generated match IDs; flags ephemeral (non-persisted) matches.
inline constexpr char kShootoutMatchIdPrefix[] = "SHT-";

class ShootoutManager {
public:
    enum class Phase : uint8_t {
        IDLE = 0,
        PROPOSAL = 1,
        BRACKET_REVEAL = 2,
        MATCH_IN_PROGRESS = 3,
        BETWEEN_MATCHES = 4,
        ENDED = 5,
        ABORTED = 6,
    };

    ShootoutManager(Player* player,
                    WirelessManager* wirelessManager,
                    RemoteDeviceCoordinator* rdc,
                    ChainManager* cdm);
    ~ShootoutManager() = default;

    // Subscribes per-cmd reliable channels on the supplied transport. Must be
    // called once before any send or receive traffic. Tests that don't stand
    // up a transport may leave this unset; sends become no-ops in that case.
    void initialize(WirelessTransport* transport);

    // Optional MatchManager injection. When set, Shootout primes the
    // MatchManager with the duelist pair on each MATCH_START so duel
    // states find a ready match. Tests leave this unset.
    void setMatchManager(MatchManager* matchManager) { matchManager_ = matchManager; }

    bool active() const;
    Phase getPhase() const;

    // Returns the set of MACs that participate in the current loop. Returns
    // empty when not in a loop and no test override is set.
    std::vector<std::array<uint8_t, 6>> getLoopMembers() const;

    // Test-only: override the loop-member set. Pass an empty vector to clear.
    void setLoopMembersForTest(const std::vector<std::array<uint8_t, 6>>& members);

    void startProposal();
    void confirmLocal();
    void sync();
    // CONFIRM payloads carry a fixed-size name field; longer names are
    // truncated, shorter ones null-padded.
    static constexpr size_t kNameLength = ::kNameLength;
    void onConfirmReceived(const uint8_t* fromMac, const char* name = nullptr);
    // Returns the display name for a MAC: the name announced during CONFIRM
    // if known, otherwise the last-two-byte MAC hex suffix.
    std::string getNameForMac(const uint8_t* mac) const;
    size_t getConfirmedCount() const;
    bool hasConfirmed(const uint8_t* mac) const;

    std::array<uint8_t, 6> getCoordinatorMac() const;
    bool isCoordinator() const;
    std::vector<std::array<uint8_t, 6>> getBracket() const;
    bool hasBye() const;

    void onBracketAckReceived(const uint8_t* fromMac, uint8_t seqId);
    uint8_t getLastBracketSeqId() const;
    size_t getBracketPendingAckCount() const;

    int getCurrentMatchIndex() const;
    std::pair<std::array<uint8_t,6>, std::array<uint8_t,6>> getCurrentMatchPair() const;
    void onMatchStartAckReceived(const uint8_t* fromMac, uint8_t seqId);

    void onBracketReceived(const std::vector<std::array<uint8_t, 6>>& bracket, uint8_t seqId);
    void onMatchStartReceived(const uint8_t* duelistA, const uint8_t* duelistB,
                              uint8_t matchIndex, uint8_t seqId);
    bool isLocalDuelist() const;
    std::array<uint8_t, 6> getOpponentMac() const;

    void reportLocalWin();
    void onMatchResultReceived(const uint8_t* winner, const uint8_t* loser,
                               uint8_t matchIndex, uint8_t seqId,
                               const uint8_t* fromMac);
    void onMatchResultAckReceived(const uint8_t* fromMac, uint8_t seqId);
    size_t getMatchResultPendingAckCount() const;
    uint8_t getLastMatchResultSeqId() const { return lastMatchResultSeqId_; }
    bool isEliminated(const uint8_t* mac) const;

    void onLocalRDCDisconnect(const uint8_t* lostMac);
    void onPeerLostReceived(const uint8_t* lostMac);

    // Fan-in slot for RDC's onDirectPeerChange. Routes disconnect transitions
    // into onLocalRDCDisconnect using the dropped peer's MAC. No-op on connect
    // since Shootout doesn't react to new direct peers mid-tournament.
    void onDirectPeerChange(SerialIdentifier port,
                            std::optional<RemoteDeviceCoordinator::Peer> previous,
                            std::optional<RemoteDeviceCoordinator::Peer> current);
    uint8_t getLastMatchStartSeqId() const;

    void onTournamentEndReceived(const uint8_t* winner, uint8_t seqId);
    void onTournamentEndAckReceived(const uint8_t* fromMac, uint8_t seqId);
    size_t getTournamentEndPendingAckCount() const;
    uint8_t getLastTournamentEndSeqId() const { return lastTournamentEndSeqId_; }
    void onAbortReceived();
    std::array<uint8_t, 6> getTournamentWinner() const;

    // Idempotent on ABORTED/ENDED phases.
    void abortTournament();

    // Reset all tournament state back to IDLE phase so a subsequent loop
    // closure triggers a fresh proposal. Called when the physical ring is
    // broken after TOURNAMENT_END or ABORTED.
    void resetToIdle();

    static constexpr unsigned long kConfirmRebroadcastMs = 1000;
    static constexpr unsigned long kBracketRevealMs = 5000;

private:
    struct NameEntry {
        std::array<uint8_t, 6> mac;
        std::string name;
    };

    WirelessTransport* transport_ = nullptr;
    ReliableChannel<ShootoutConfirmPayload, ShootoutCmd>* confirmChannel_ = nullptr;
    ReliableChannel<ShootoutBracketEntryPayload, ShootoutCmd>* bracketEntryChannel_ = nullptr;
    ReliableChannel<ShootoutMatchStartPayload, ShootoutCmd>* matchStartChannel_ = nullptr;
    ReliableChannel<ShootoutMatchResultPayload, ShootoutCmd>* matchResultChannel_ = nullptr;
    ReliableChannel<ShootoutTournamentEndPayload, ShootoutCmd>* tournamentEndChannel_ = nullptr;
    ReliableChannel<ShootoutPeerLostPayload, ShootoutCmd>* peerLostChannel_ = nullptr;
    ReliableChannel<ShootoutAbortPayload, ShootoutCmd>* abortChannel_ = nullptr;

    // Per-batch buffering for inbound BRACKET_ENTRY. Each batchId reassembles
    // into the local bracket_ view once every slot has arrived.
    struct PendingBracketBatch {
        // True only while a batch is mid-assembly. On completion the buffer is
        // std::move'd into bracket_ and this is cleared, so a retransmitted entry
        // for the just-finished batchId re-initializes (active==false path at
        // onBracketEntryReceived) instead of indexing the moved-from, now-empty
        // buffer — which would be an out-of-bounds write. Also separates the
        // initial state (batchId defaults to 0) from assembling batch 0.
        bool active = false;
        uint8_t batchId = 0;
        uint8_t totalSlots = 0;
        uint8_t seqId = 0;        // seqId of the first entry, replayed on full-batch ack-already-sent.
        std::vector<std::array<uint8_t, 6>> buffer;
        std::vector<bool> receivedSlots;
    };
    PendingBracketBatch pendingBracket_;
    uint8_t nextBracketBatchId_ = 1;
    static bool batchIsNewer(uint8_t a, uint8_t b);

    void onBracketEntryReceived(const uint8_t* fromMac,
                                const ShootoutBracketEntryPayload& p);

    // Set when Resender abandons BRACKET, MATCH_START, or MATCH_RESULT.
    // Deferred so abortTournament's pending-list mutations don't run inside
    // the Resender::sync() callback chain.
    bool tournamentAbortPending_ = false;
    Player* player_;
    WirelessManager* wirelessManager_;
    RemoteDeviceCoordinator* rdc_;
    ChainManager* cdm_;
    MatchManager* matchManager_ = nullptr;
    Phase phase_ = Phase::IDLE;

    void primeMatchManagerForMatch();

    std::vector<std::array<uint8_t, 6>> testLoopMembers_;
    bool testLoopMembersOverride_ = false;
    std::vector<std::array<uint8_t, 6>> confirmedSet_;

    std::vector<NameEntry> names_;
    void recordName(const uint8_t* mac, const char* name);

    std::vector<std::array<uint8_t, 6>> buildLoopMemberSet() const;
    void sendLocalConfirm();
    bool allMembersConfirmed() const;
    void advanceToBracketReveal();
    void generateBracket();

    std::vector<std::array<uint8_t, 6>> bracket_;
    // Coord-only working set: starts equal to bracket_, replaced by survivors at
    // each round boundary. bracket_ stays immutable so getBracket() remains
    // stable and non-coord position lookups don't desync.
    std::vector<std::array<uint8_t, 6>> currentRound_;

    SimpleTimer confirmRebroadcastTimer_;

    uint8_t lastBracketSeqId_ = 0;

    void sendBracketToPeers();

    static std::array<uint8_t, 6> lowestMacIn(
        const std::vector<std::array<uint8_t, 6>>& set);

    // Stable post-bracket-formation. Used in place of lowestMacIn(bracket_)
    // which would otherwise rescan the bracket on every ack path.
    std::array<uint8_t, 6> coordinatorMac_{};

    std::array<uint8_t, 6> opponentMac_{};

    int currentMatchIndex_ = -1;
    // bracket_ shrinks on round advancement (coordinator only), so cache the
    // duelist pair separately to stay valid on non-coordinators too.
    std::array<uint8_t, 6> currentDuelistA_{};
    std::array<uint8_t, 6> currentDuelistB_{};
    uint8_t lastMatchStartSeqId_ = 0;
    SimpleTimer bracketRevealTimer_;
    void maybeStartNextMatch();
    bool inMaybeStartNextMatch_ = false;
    void sendMatchStartToPeers(int matchIndex);

    std::vector<std::array<uint8_t, 6>> eliminated_;
    bool isActiveDuelist(const uint8_t* mac) const;
    bool isSameMatch(int matchIndex, const uint8_t* a, const uint8_t* b) const;
    bool reportedLocalWin_ = false;
    uint8_t lastMatchResultSeqId_ = 0;
    void sendMatchResultToPeers(const uint8_t* winner, const uint8_t* loser,
                              uint8_t matchIndex);
    void applyMatchResult(const uint8_t* winner, const uint8_t* loser);

    std::array<uint8_t, 6> tournamentWinner_{};
    uint8_t lastTournamentEndSeqId_ = 0;

    void sendTournamentEndToPeers(const uint8_t* winner);
    std::array<uint8_t, 6> findLastRemaining() const;
};
