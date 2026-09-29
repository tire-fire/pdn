#pragma once

#include <array>
#include <cstdint>
#include <vector>
#include "game/player.hpp"
#include "device/remote-device-coordinator.hpp"
#include "wireless/resender.hpp"
#include "device/drivers/peer-comms-types.hpp"
#include "device/drivers/entropy-interface.hpp"
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

    /// Takes no RDC subscription: the ring latch is polled where it is used, because a
    /// closure edge can be resolved away before anyone acts on it.
    /// `entropy` supplies each attempt's identity. Required, and required to survive a
    /// reset: a device whose identities repeat after a reboot accepts a peer's confirm
    /// from the attempt before it, counts that peer into a bracket it never joined, and
    /// waits on it forever.
    ShootoutManager(Player* player,
                    WirelessManager* wirelessManager,
                    RemoteDeviceCoordinator* rdc,
                    EntropyInterface* entropy);
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

    /// Decodes one kShootoutCommand frame and routes it to the handler for its
    /// command byte. The wire layout lives here beside the builders that write it:
    /// GameSession and the multi-device test fixture both route through this, so a
    /// frame the hardware would reject is rejected in the tests too.
    void onShootoutFrame(const uint8_t* fromMac, const uint8_t* data, size_t dataLen);
    /// Decodes one kShootoutCommandAck frame. The command byte is not read at all:
    /// the resender matches an ack on its channel, seqId and sender, so the seqId
    /// alone names the frame being answered.
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
    /// Minted by the head when its proposal opens and adopted from RING_CLOSED by
    /// everyone else, so it names one attempt fleet-wide without any agreement step.
    /// A device that comes to head the ring mid-proposal announces under the one it
    /// adopted rather than minting a second; if it adopted none, sync() mints before it
    /// announces, so nothing ever goes out under 0.
    uint32_t getTournamentEpoch() const { return tournamentEpoch; }
    /// True while this device sits on a closed ring and no tournament is running:
    /// the Idle -> ShootoutProposal transition predicate.
    bool shouldEnterProposal() const;

    void startProposal();
    void confirmLocal();
    void sync();
    // [cmd, seqId, 4-byte attempt identity] — every kShootoutCommand frame opens with
    // this. writeHeader writes it and onShootoutFrame reads it, so the layout has one
    // owner on each side of the wire. The ack frame is the exception: it is a
    // ShootoutAckPayload, and the seqId alone names the frame it answers.
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
    /// How long a device waits, from its own press, for a roster above the floor to
    /// finish confirming. Ring detection can serve a name for a device that has
    /// already left, and that name answers nothing, so an absent confirm is the only
    /// signal there is. On a member this is the only bound there is, at any roster
    /// size: the short-roster path below is head-gated. Paced for players: firing early
    /// costs a screen they can retry, never firing costs a ring that hangs until
    /// someone unplugs it.
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
    EntropyInterface* entropy;
    // The attempt every frame this device sends is stamped with, and the one it
    // accepts frames from. Deliberately outside resetTournamentState: a member
    // adopts it from RING_CLOSED before it enters the proposal that resets
    // everything else, and it has to survive that.
    uint32_t tournamentEpoch = 0;
    // Why this device is in ABORTED: the ring told it to, rather than its own bound
    // running out. Only the first kind refuses a bracket that names it.
    bool abortedByRing = false;

    void primeMatchManagerForMatch();
    // Drops what an attempt leaves behind and keeps the two things that outlive one:
    // the ring roster and the attempt identity. resetToIdle is the one that ends a
    // tournament and clears those too.
    void leaveAttempt();
    // leaveAttempt() invalidates the ring-closed rebroadcast timer on top of this,
    // which is what makes sync()'s never-announced case fire for the next attempt;
    // resetToIdle() drops the ring roster as well, and startProposal() reseeds it from
    // ring detection instead.
    void resetTournamentState();

    uint8_t nextSeqId();
    // Mints the identity of a new attempt, straight from the entropy source, so it
    // separates attempts, devices and reboots with no agreement step and nothing stored.
    // Distinctness is only as good as the bar EntropyInterface sets, which is what makes
    // a collision between two live tournaments not worth designing against rather than
    // impossible. A counter covered the first axis only: it restarts at boot, and
    // the failure that follows is a peer's confirm from the attempt before the reset
    // counting toward the one after it. Never returns 0, which means "no attempt".
    uint32_t mintEpoch();
    // Writes the header into `out`, which must hold at least kHeaderLength bytes, and
    // returns where the payload starts. Every frame is stamped with the attempt this
    // device is in, including the ABORT that leaves one: giveUpLocally reaches for
    // leaveAttempt, which keeps the identity standing.
    size_t writeHeader(uint8_t* out, ShootoutCmd cmd, uint8_t seqId) const;
    // The body MATCH_START and MATCH_RESULT share, after the header: two MACs and the
    // match index. One declaration, so the builders, the decoder and the abandon
    // path's re-read of a kept frame cannot drift apart on an offset.
    struct MatchPairBody {
        uint8_t a[6];
        uint8_t b[6];
        uint8_t index;
    } __attribute__((packed));
    static_assert(sizeof(MatchPairBody) == 13,
                  "MATCH_START/MATCH_RESULT bodies are 13 bytes on the wire");
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
    // of its confirmed set, so none of the three subsumes the others. The loop
    // roster outlives a give-up, so a device that gave up and then took its head's
    // bracket back is covered by it — reaching past these to whatever ring detection
    // calls head right now would also let one coordinator's bracket be aborted by a
    // different device, which is the disagreement this is meant to prevent.
    bool isRingMember(const uint8_t* mac) const;
    void broadcastCommand(const uint8_t* packet, size_t len);
    void broadcastToRing(const std::vector<std::array<uint8_t, 6>>& audience,
                         const uint8_t* packet, size_t len);
    // Leaves the tournament on this device alone, with no fan-out: ABORTED for the
    // player, and nothing said to the ring.
    void giveUpLocally();
    // Drops what the local press bought for one attempt, leaving the phase alone.
    void forgetAttemptConsent();
    // Takes a ring's roster and the attempt it belongs to, dropping the consent that
    // belonged to the last one. Every branch of onRingClosedReceived that joins an
    // announcement ends here, so the three cannot drift apart on what joining means.
    void adoptAttempt(const std::vector<std::array<uint8_t, 6>>& members, uint32_t epoch);
    /// The peers a ring fan-out is addressed to: `peers` without this device.
    std::vector<std::array<uint8_t, 6>> peersExcludingSelf(
        const std::vector<std::array<uint8_t, 6>>& peers) const;
    void sendReliablyToPeers(const std::vector<std::array<uint8_t, 6>>& peers,
                             uint8_t seqId, const uint8_t* packet, size_t len);
    std::vector<std::array<uint8_t, 6>> testLoopMembers;
    bool testLoopMembersOverride = false;
    std::vector<std::array<uint8_t, 6>> confirmedSet;

    // The tournament's roster, and the one the head announced. On a head: read from
    // ring detection when the proposal starts and grown from it once per tick, so it
    // never shrinks inside one attempt. On every other member: taken from the head's
    // RING_CLOSED, which is also what detection serves back to a member, so the seed
    // and the grow are both no-ops there.
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

    // Retransmits the five command families that are sent reliably; CONFIRM and
    // RING_CLOSED are repeated on a timer instead and never reach here. Owned by this
    // manager, not shared with the coordinator's: a fan-out armed here must die with it
    // rather than keep broadcasting for a tournament that is over. All five ride one
    // PktType, so the abandon callback reads which one gave up off the command byte.
    Resender resender;
    void onCommandAbandoned(uint8_t seqId, const uint8_t* targetMac,
                            const uint8_t* packet, size_t len);

    void sendBracketToPeers();
    // [header, count, count * 6-byte MAC] — the frame BRACKET and RING_CLOSED share.
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
    // The match whose result this device has already re-sent once, or -1. The index alone
    // cannot name a bout — the coordinator restarts it at 0 each round — so this is
    // cleared wherever a device learns of a different bout, which for a member is
    // onMatchStartReceived and for anyone adopting a bracket is that.
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
    /// repaired by the next copy whatever the loss count. Repetition rather than a
    /// watchdog because giving up on the ending is the abandonment nobody notices: the
    /// sender is already terminal, so it waits on nothing while the member waits
    /// forever.
    void reannounceEnding();
};
