#pragma once

#include <array>
#include <cstdint>
#include <vector>
#include "game/player.hpp"
#include "device/remote-device-coordinator.hpp"
#include "wireless/resender.hpp"
#include "device/drivers/peer-comms-types.hpp"
#include "device/wireless-manager.hpp"
#include "utils/debounced-condition.hpp"
#include "utils/simple-timer.hpp"

class MatchManager;

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

    /// Subscribes to the coordinator's ring-closed edge when given one. That
    /// callback is a single slot, so at most one ShootoutManager per coordinator:
    /// a second built on the same one takes the slot over, and whichever is
    /// destroyed first empties it for both.
    ShootoutManager(Player* player,
                    WirelessManager* wirelessManager,
                    RemoteDeviceCoordinator* rdc);
    /// Drops the coordinator subscriptions the constructor took, which hold `this`.
    ~ShootoutManager();

    /// Optional MatchManager injection. When set, Shootout primes the
    /// MatchManager with the duelist pair on each MATCH_START so duel
    /// states find a ready match. Tests leave this unset.
    void setMatchManager(MatchManager* manager) { matchManager = manager; }

    bool active() const;
    Phase getPhase() const;

    // Returns the set of MACs that participate in the current loop. Returns
    // empty when not in a loop and no test override is set.
    std::vector<std::array<uint8_t, 6>> getLoopMembers() const;

    // Test-only: override the loop-member set. Pass an empty vector to clear.
    void setLoopMembersForTest(const std::vector<std::array<uint8_t, 6>>& members);

    /// RDC ring-closed observer: snapshots the ring roster and announces it to the
    /// other members. Who coordinates is read from the RDC when a bracket is drawn.
    void onRingClosed();
    /// Decodes one kShootoutCommand frame and routes it to the handler for its
    /// command byte. The wire layout lives here beside the builders that write it:
    /// GameSession and the multi-device test fixture both route through this, so a
    /// frame the hardware would reject is rejected in the tests too.
    void onShootoutFrame(const uint8_t* fromMac, const uint8_t* data, size_t dataLen);
    /// Decodes one kShootoutCommandAck frame. The seqId alone names the frame, so
    /// the command byte is only range-checked, never dispatched on.
    void onShootoutAckFrame(const uint8_t* fromMac, const uint8_t* data, size_t dataLen);
    /// Inbound RING_CLOSED: records `members` as the ring roster, which a member
    /// gates confirms against, and `epoch` as the attempt they belong to. A member
    /// has no other proposal trigger, though it must also still be on the ring when
    /// the gate is polled. This is the only frame that legitimately crosses
    /// attempts: it is how a device learns of the next one, and how it learns the
    /// one it is holding is over.
    void onRingClosedReceived(const uint8_t* fromMac,
                              const std::vector<std::array<uint8_t, 6>>& members,
                              uint32_t epoch);
    /// The attempt this device is taking part in, or 0 before it has joined one.
    /// Minted by the head that announces the ring and adopted from RING_CLOSED by
    /// everyone else, so it names one attempt fleet-wide without any agreement step.
    uint32_t getTournamentEpoch() const { return tournamentEpoch; }
    /// True while this device sits on a closed ring and no tournament is running:
    /// the Idle -> ShootoutProposal transition predicate.
    bool shouldEnterProposal() const;

    void startProposal();
    /// Leaves the tournament on this device alone, with no fan-out: ABORTED for the
    /// player, and nothing said to the ring.
    void giveUpLocally();
    void confirmLocal();
    void sync();
    // [cmd, seqId, 4-byte attempt identity] — every shootout frame opens with this.
    // writeHeader writes it and onShootoutFrame reads it, so the layout has one
    // owner on each side of the wire.
    static constexpr size_t kHeaderLength = 6;
    // Fixed on-wire name length for CONFIRM payloads. Local Player names are
    // null-padded/truncated to this size.
    static constexpr size_t kNameLength = 12;
    void onConfirmReceived(const uint8_t* fromMac, const char* name = nullptr);
    // Returns the display name for a MAC: the name announced during CONFIRM
    // if known, otherwise the last-two-byte MAC hex suffix.
    std::string getNameForMac(const uint8_t* mac) const;
    size_t getConfirmedCount() const;
    bool hasConfirmed(const uint8_t* mac) const;

    std::array<uint8_t, 6> getCoordinatorMac() const;
    /// True when this device drew the bracket it holds.
    bool isCoordinator() const;
    std::vector<std::array<uint8_t, 6>> getBracket() const;

    /// Cumulative retry counters for this manager's command channel. Sends and
    /// retries count frames, abandons count recipients; see Resender::Stats.
    const Resender::Stats& getRetryStats() const { return resender.getStats(); }

    /// Recipients of the fan-out sent under `seqId` that have not yet acked.
    /// Zero once every one of them has answered or been given up on.
    size_t getPendingAckCount(uint8_t seqId) const;
    uint8_t getLastBracketSeqId() const;

    int getCurrentMatchIndex() const;
    /// The two devices of the current match index. Zero MACs only before the
    /// first match; the index is not cleared at a match boundary, so between
    /// matches and after the tournament ends this still names the last pair.
    std::pair<std::array<uint8_t, 6>, std::array<uint8_t, 6>> getCurrentMatchPair() const;

    /// Adopts the bracket the RDC's ring head drew and acks it. Once a bracket is
    /// held, its author's retransmits are acked and ignored, and a different bracket
    /// from that author is a new tournament. Anything else is refused, unacked.
    void onBracketReceived(const uint8_t* fromMac,
                           const std::vector<std::array<uint8_t, 6>>& offeredBracket,
                           uint8_t seqId);
    /// Inbound MATCH_START. Admitted on `fromMac` being our coordinator, which
    /// is the authority BRACKET propagated; the duelist pair is game content and
    /// cannot answer whether a broadcast frame is ours.
    void onMatchStartReceived(const uint8_t* fromMac,
                              const uint8_t* duelistA, const uint8_t* duelistB,
                              uint8_t matchIndex, uint8_t seqId);
    bool isLocalDuelist() const;
    std::array<uint8_t, 6> getOpponentMac() const;

    void reportLocalWin();
    /// An ack for one of this manager's fan-outs, named by the seqId it answers.
    void onCommandAckReceived(const uint8_t* fromMac, uint8_t seqId);

    void onMatchResultReceived(const uint8_t* winner, const uint8_t* loser,
                               uint8_t matchIndex, uint8_t seqId,
                               const uint8_t* fromMac);
    /// seqId of the MATCH_RESULT this device most recently sent.
    uint8_t getLastMatchResultSeqId() const { return lastMatchResultSeqId; }
    bool isEliminated(const uint8_t* mac) const;

    uint8_t getLastMatchStartSeqId() const;

    /// Inbound TOURNAMENT_END, admitted on `fromMac` for the same reason as
    /// onMatchStartReceived.
    void onTournamentEndReceived(const uint8_t* fromMac, const uint8_t* winner,
                                 uint8_t seqId);
    /// seqId of the TOURNAMENT_END this device most recently sent.
    uint8_t getLastTournamentEndSeqId() const { return lastTournamentEndSeqId; }
    /// Tears down on a peer's ABORT. fromMac identifies the sending ring: the
    /// command carries no MACs of its own, and a broadcast reaches every ring
    /// in radio range.
    void onAbortReceived(const uint8_t* fromMac, uint8_t seqId);
    std::array<uint8_t, 6> getTournamentWinner() const;

    // Reset all tournament state back to IDLE phase so a subsequent loop
    // closure triggers a fresh proposal. Called when the physical ring is
    // broken after TOURNAMENT_END or ABORTED.
    void resetToIdle();

    /// Tears down, lands in Phase::ABORTED, and only then fans ABORT out to the
    /// ring (bracket, or confirmedSet before a bracket exists). Armed after the
    /// teardown because the teardown cancels the fan-outs in flight, sparing only
    /// the seqId already recorded as terminal — which this one is not yet.
    /// Idempotent, and refuses ENDED as well as ABORTED.
    void abortTournament();

    /// Ring-break debounce. A cable nudge flickers the loop on real hardware; act
    /// only once the break has settled. This covers the flicker, not a recovery:
    /// healing a nudge costs a link re-handshake, the latching device's own MAC
    /// walking the loop to re-latch, then the flag walking back, all of which
    /// outlast it. Calibrated against how long a nudge flickers, so it is not the
    /// RDC's propagation window even though both are 500 — widening this one is a
    /// rig question and does not imply widening that one.
    static constexpr unsigned long LOOP_BREAK_DEBOUNCE_MS = 500;
    /// Confirmed members needed to draw a bracket. Two is the structural floor —
    /// a duel needs two duelists — so a two-device ring is exactly at it and runs.
    static constexpr size_t MIN_PARTICIPANTS = 2;
    /// How long a roster stays below MIN_PARTICIPANTS before the proposal is
    /// given up on. Wall-clock, and generous: what fills the roster is members
    /// announcing themselves to the head, which nothing here drives or can
    /// predict. Raise it if a venue's larger rings are seen to fill slower.
    static constexpr unsigned long SHORT_ROSTER_TIMEOUT_MS = 3000;
    /// How long the ring waits for a roster to finish confirming. Paced for
    /// players, not for frames: a ring detection can serve a name for a device that
    /// has already left, and that name answers nothing, so the absence of a confirm
    /// is the only signal there is: the ring is intact, so the break guard stays
    /// quiet, and a peer's own ABORT is the only other way out. SHORT_ROSTER_TIMEOUT_MS
    /// gets there faster when the roster is below the floor, which is the case it
    /// watches; this one is for a roster above it that still cannot complete.
    /// Generous because the cost of firing early is a screen the players can retry,
    /// and the cost of never firing is a ring that hangs until someone unplugs it.
    /// Gated on a local confirm, so an untouched ring sits idle instead of looping.
    static constexpr unsigned long PROPOSAL_TIMEOUT_MS = 60000;
    static constexpr unsigned long kConfirmRebroadcastMs = 1000;
    static constexpr unsigned long kBracketRevealMs = 5000;
    // Packet-validation clamp on an inbound BRACKET's member count. A ring can
    // hold as many devices as the chain does, so it tracks MAX_CHAIN_MEMBERS;
    // one ESP-NOW v2 frame carries that bracket several times over.
    static constexpr uint8_t MAX_BRACKET_SIZE = MAX_CHAIN_MEMBERS;

private:
    struct NameEntry {
        std::array<uint8_t, 6> mac;
        std::string name;
    };

    Player* player;
    WirelessManager* wirelessManager;
    RemoteDeviceCoordinator* rdc;
    MatchManager* matchManager = nullptr;
    Phase phase = Phase::IDLE;
    // The attempt every frame this device sends is stamped with, and the one it
    // accepts frames from. Deliberately outside resetTournamentState: a member
    // adopts it from RING_CLOSED before it enters the proposal that resets
    // everything else, and it has to survive that.
    uint32_t tournamentEpoch = 0;
    uint8_t epochCounter = 0;

    void primeMatchManagerForMatch();
    // resetToIdle() clears the ring roster on top of this; startProposal() reseeds
    // it from ring detection instead.
    void resetTournamentState();

    uint8_t nextSeqId();
    // Mints the identity of a new attempt: the low three bytes of this device's MAC
    // name the announcer, the counter names the attempt. Unique without any
    // agreement step, which is the whole point — two devices that close rings at the
    // same moment must not mint the same value. Never returns 0, which means "no
    // attempt", unless this device has no MAC to name itself with.
    uint32_t mintEpoch();
    // Writes [cmd, seqId, attempt identity] into `out`, which must hold at least
    // kHeaderLength bytes. Returns where the payload starts.
    size_t writeHeader(uint8_t* out, ShootoutCmd cmd, uint8_t seqId) const;
    // [header, 6-byte winner]. `out` must hold kHeaderLength + 6 bytes. Shared by
    // the reliable fan-out and the unreliable repeat, which differ only in seqId.
    void buildTournamentEndPacket(uint8_t* out, const uint8_t* winner, uint8_t seqId) const;
    static bool containsMac(const std::vector<std::array<uint8_t, 6>>& set,
                            const uint8_t* mac);
    /// Appends `mac` unless it is already there. The write half of containsMac.
    static void addMac(std::vector<std::array<uint8_t, 6>>& set, const uint8_t* mac);
    /// True when `mac` is the coordinator this device is following.
    bool isFromCoordinator(const uint8_t* mac) const;
    /// True in the two phases a tournament ends in. Every handler that would
    /// advance a tournament refuses them, so a late frame cannot reopen one; the
    /// two terminal screens end them deliberately, via resetToIdle on dismount.
    /// ENDED leaves the coordinator anchor and the bracket standing, so its
    /// coordinator's frames still pass the sender checks; an abort clears both,
    /// so a retransmitted BRACKET would otherwise be adopted afresh.
    bool isTerminalPhase() const {
        return phase == Phase::ENDED || phase == Phase::ABORTED;
    }
    // Any of the three, because which set knows the ring depends on the phase:
    // the bracket after reveal, the confirmed set during the proposal, the
    // physical loop before either exists. A follower's bracket is not a subset
    // of its confirmed set, so none of the three subsumes the others.
    bool isRingMember(const uint8_t* mac) const;
    void broadcastCommand(const uint8_t* packet, size_t len);
    void broadcastToRing(const std::vector<std::array<uint8_t, 6>>& peers,
                         const uint8_t* packet, size_t len);
    /// The peers a ring fan-out is addressed to: `peers` without this device.
    std::vector<std::array<uint8_t, 6>> peersExcludingSelf(
        const std::vector<std::array<uint8_t, 6>>& peers) const;
    void sendReliablyToPeers(const std::vector<std::array<uint8_t, 6>>& peers,
                             uint8_t seqId, const uint8_t* packet, size_t len);
    std::vector<std::array<uint8_t, 6>> testLoopMembers;
    bool testLoopMembersOverride = false;
    std::vector<std::array<uint8_t, 6>> confirmedSet;

    // The tournament's roster, and the one the head announced. Seeded from ring
    // detection when a proposal starts, grown from it once per tick while the
    // proposal runs, and on every other member taken from the head's RING_CLOSED.
    // Non-empty is also the "a ring closed" latch, and nothing retires it while
    // IDLE, so shouldEnterProposal pairs it with a liveness check.
    std::vector<std::array<uint8_t, 6>> ringMembers;
    DebouncedCondition ringBreakDebounce;
    DebouncedCondition shortRosterDebounce;
    SimpleTimer ringClosedRebroadcastTimer;
    void sendRingClosed();

    std::vector<NameEntry> names;
    void recordName(const uint8_t* mac, const char* name);

    /// True while the RDC has this device latched as the ring's head.
    bool headsRing() const;
    /// The ring's head as the RDC reports it: this device when latched, else the
    /// head relayed round the ring. nullptr off a ring or before one propagates.
    const uint8_t* ringHead() const;
    std::vector<std::array<uint8_t, 6>> buildLoopMemberSet() const;
    void sendLocalConfirm();
    bool allMembersConfirmed(const std::vector<std::array<uint8_t, 6>>& members) const;
    /// Draws the bracket and fans it out, on the device ring detection has as the
    /// ring's head. A no-op anywhere else: a device with nothing to show stays in
    /// the proposal, where it goes on confirming. Called only from sync(), on the
    /// main loop — a caller on the packet path would draw off a head latch sampled
    /// at whatever instant the frame arrived.
    void drawBracket();
    void generateBracket();

    std::vector<std::array<uint8_t, 6>> bracket;
    // Coord-only working set: starts equal to bracket, replaced by survivors at
    // each round boundary. bracket stays immutable so getBracket() remains
    // stable and non-coord position lookups don't desync.
    std::vector<std::array<uint8_t, 6>> currentRound;

    SimpleTimer confirmRebroadcastTimer;
    SimpleTimer endingRebroadcastTimer;
    SimpleTimer proposalTimer;

    uint8_t lastBracketSeqId = 0;
    uint8_t nextShootoutSeqId = 1;

    // Retransmits for every command family this manager sends. Owned here, not
    // shared with the coordinator's: a fan-out armed by this manager must die
    // with it rather than keep broadcasting for a tournament that is over.
    // All five families ride one PktType, so the abandon callback reads which
    // one gave up off the frame's own command byte.
    Resender resender;
    void onCommandAbandoned(uint8_t seqId, const uint8_t* targetMac,
                            const uint8_t* packet, size_t len);

    void sendBracketToPeers();
    // [cmd, seqId, count, count * 6-byte MAC] — the frame BRACKET and
    // RING_CLOSED share.
    std::vector<uint8_t> buildMacListPacket(
        ShootoutCmd cmd, uint8_t seqId,
        const std::vector<std::array<uint8_t, 6>>& macs) const;

    // The author of the bracket held: self on the head that drew it, the sender on
    // every other member. All-zero while no bracket is held.
    std::array<uint8_t, 6> coordinatorMac{};

    std::array<uint8_t, 6> opponentMac{};
    void sendShootoutAck(ShootoutCmd cmd, uint8_t seqId, const uint8_t* toMac);

    int currentMatchIndex = -1;
    // bracket shrinks on round advancement (coordinator only), so cache the
    // duelist pair separately to stay valid on non-coordinators too.
    std::array<uint8_t, 6> currentDuelistA{};
    std::array<uint8_t, 6> currentDuelistB{};
    uint8_t lastMatchStartSeqId = 0;
    SimpleTimer bracketRevealTimer;
    /// Advances the bracket. Called only from sync(), on the main loop — a second
    /// caller on the packet path would need its own guard against double-advance.
    void maybeStartNextMatch();
    void sendMatchStartToPeers(int matchIndex);
    std::vector<uint8_t> buildMatchStartPacket(int matchIndex) const;

    std::vector<std::array<uint8_t, 6>> eliminated;
    bool isSameMatch(int matchIndex, const uint8_t* a, const uint8_t* b) const;
    bool reportedLocalWin = false;
    // The match whose result this device has already re-sent once, or -1. Keyed
    // on the bout rather than a flag, so it needs no clearing: a later match
    // names a different index and gets its own attempt. A device flag would have
    // to be reset wherever a match turns over, which differs by role.
    int matchResultResentIndex = -1;
    uint8_t lastMatchResultSeqId = 0;
    void sendMatchResultToPeers(const uint8_t* winner, const uint8_t* loser,
                                uint8_t matchIndex);
    /// Applies an elimination. `endsCurrentBout` false records it without
    /// leaving MATCH_IN_PROGRESS — a late result for an older bout must not pull
    /// this device out of the one it is fighting now.
    void applyMatchResult(const uint8_t* winner, const uint8_t* loser,
                          bool endsCurrentBout = true);
    std::vector<uint8_t> buildMatchResultPacket(const uint8_t* winner,
                                                const uint8_t* loser,
                                                uint8_t matchIndex) const;

    std::array<uint8_t, 6> tournamentWinner{};
    uint8_t lastTournamentEndSeqId = 0;
    // The ABORT or TOURNAMENT_END this device sent to report the tournament
    // ending, which a teardown spares — see resetTournamentState. Zero until one
    // is armed, and zero on a device that only ever received the news.
    uint8_t terminalFanOutSeqId = 0;

    void sendTournamentEndToPeers(const uint8_t* winner);
    /// Puts the ending back on the air as an unreliable broadcast, seqId 0. Repeats
    /// while this device shows the winner, so a member that missed the fan-out is
    /// repaired by the next copy whatever the loss count.
    void reannounceEnding();
};
