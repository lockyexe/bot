#pragma once

// Pathfinder: a trial-and-error search that runs inside Practice Mode.
//
// Every checkpoint starts a "segment". A segment owns an input plan: a list
// of press/release events at absolute physics ticks (m_currentProgress, which
// practice checkpoints save and restore, so replays line up tick-exactly).
//
//  * Up to `lockedUntil` the plan is replayed exactly (fast-forwarded when
//    the part is already known to be safe).
//  * After that a live policy plays and every toggle it makes is appended.
//  * A death opens a "cluster": the plan before a look-back window is kept
//    and candidates for the window are tried, simplest first. Results steer
//    the next try towards whatever got furthest (progress-guided search), a
//    stuck detector escalates early, and an exact frame sweep runs in Mode 3.
//  * Safe progress places checkpoints (re-checked at the moment of placing).
//    A checkpoint with too many failures is deleted, its exact player state
//    is remembered as a dead end, and the parent segment must find another
//    way in.
//  * Solved clicks are later nudged towards a natural spot inside the
//    window they were found to work in (polish pass), kept only if the run
//    still survives.
//
// All clicks go through GJBaseGameLayer::handleButton, the same entry point
// the game's own input uses, so macro recorders capture them as player input.

#include <Geode/Geode.hpp>

#include <map>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "Humanizer.hpp"

namespace pf {

enum class Mode : uint8_t { Cube, Ship, Ball, Ufo, Wave, Robot, Spider, Swing };
char const* modeName(Mode m);
bool isFlyMode(Mode m);

enum class ObjKind : uint8_t { Solid, Hazard, Orb, Pad, Portal };

struct Event {
    uint64_t tick = 0;
    bool hold = false;
    uint8_t ch = 0; // input channel: 0 = player 1, 1 = player 2 (2-player levels only)
};

struct PlayerSnap {
    bool valid = false;
    float x = 0, y = 0;
    float vy = 0;          // measured world-space vertical speed, units per tick
    float vx = 0;          // measured horizontal speed, units per tick
    double yVel = 0;       // m_yVelocity
    Mode mode = Mode::Cube;
    bool upsideDown = false;
    bool onGround = false;
    bool dead = false;
    bool mini = false;
    float size = 1.f;      // m_vehicleSize
    cocos2d::CCRect rect;
};

struct TraceTick {
    float x = 0, y = 0, vy = 0;
    bool onGround = false;
    bool hold = false;
    bool touchingOrb = false;
    bool upsideDown = false;
    Mode mode = Mode::Cube;
};

struct Trace {
    uint64_t base = 0;                 // tick of ticks[ch][0]
    std::vector<TraceTick> ticks[2];
    TraceTick const* at(uint8_t ch, uint64_t tick) const;
    TraceTick const* last(uint8_t ch) const;
};

// Parameters for the flying-mode corridor controller.
struct FlyParams {
    float centerOffset = 0.f; // shift of the comfortable line (units, + = up)
    float bandScale = 1.f;    // scale of the hysteresis band
    float leadScale = 1.f;    // how far ahead the controller predicts
};

struct FlyOverride {
    uint64_t from = 0, to = 0;
    uint8_t ch = 0;
    FlyParams params;
};

struct Candidate {
    std::vector<Event> events; // events inside the window (explicit candidates)
    bool live = false;         // live candidate: let the controller fly the window with `fly`
    FlyParams fly;
    double score = 0;          // lower = tried first
    uint64_t key = 0;          // dedupe key
    int64_t press = -1;        // first press inside the window (-1: none)
    int hold = 0;              // its hold length
};

struct TryResult {
    int64_t press = -1;
    int hold = 0;
    uint64_t outcome = 0;      // tick the try died on
};

struct Cluster {
    uint64_t deathTick = 0;
    uint64_t windowStart = 0;
    uint8_t ch = 0;
    Mode mode = Mode::Cube;
    int side = 0;              // +1: what killed us was above, -1: below, 0: unknown
    bool doomed = false;       // opened by backtracking out of a dead checkpoint
    std::vector<Event> base;   // plan before the window
    std::vector<Candidate> queue;
    size_t next = 0;
    int tries = 0;
    int widen = 0;
    bool pairsAdded = false;
    bool microAdded = false;
    Trace trace;               // the attempt that died here
    std::vector<Event> attemptPlan; // full plan of that attempt

    // Progress-guided search / stuck detector
    std::vector<TryResult> results;
    uint64_t bestDeath = 0;    // furthest any try got
    int triesSinceImprove = 0;
    int escalation = 0;
    Candidate current;         // the candidate being tried right now
    bool hasCurrent = false;
    bool resolvedNoted = false;

    // Exact frame sweep (Mode 3)
    float deathX = 0;
    int nearCrashes = 0;       // crashes within ±5 units of deathX
    bool sweepAdded = false;
    int64_t bestPress = -1;    // press tick of the try that got furthest
    int bestHold = 0;
};

// A solved click being moved to a more natural spot inside its window.
struct PolishItem {
    uint64_t orig = 0;  // press tick that is known to work
    uint64_t alt = 0;   // more natural press tick being tested
    int hold = 0;
    uint8_t ch = 0;
    uint64_t ref = 0;   // tick of the obstacle it got us past
};

struct Segment {
    uint64_t startTick = 0;
    bool startHold[2] = {false, false};
    std::vector<Event> plan;
    uint64_t lockedUntil = 0;
    std::optional<Cluster> cluster;
    std::optional<FlyOverride> flyOverride;
    int failures = 0;
    int attempts = 0;
    std::unordered_set<uint64_t> tried;
    std::unordered_set<uint64_t> blacklist;
    Trace lastTrace;
    HumanParams human;
    bool ours = true;          // placed by the solver (false: you placed it by hand)
    TraceTick startState;      // player state when the checkpoint was placed
    bool hasStartState = false;
    std::optional<PolishItem> pendingPolish;
    std::optional<PolishItem> activePolish;
    int divergences = 0;
    bool noFastForward = false;
    int rootRounds = 0;
};

// Exact player state at a checkpoint that turned out to be a dead end.
struct DoomedState {
    float x = 0, y = 0, vy = 0;
    Mode mode = Mode::Cube;
    bool upsideDown = false;
};

struct WorldObject {
    float minX = 0, maxX = 0;
    GameObject* obj = nullptr;
    ObjKind kind = ObjKind::Solid;
};

struct Corridor {
    float floor = 0;   // top of what is below the player
    float ceil = 0;    // bottom of what is above the player
    float yMin = 0;    // lowest safe centre y
    float yMax = 0;    // highest safe centre y
    bool valid = false;
};

class Pathfinder {
public:
    static Pathfinder& get();

    bool enabled() const { return m_enabled; }
    void setEnabled(bool on, PlayLayer* pl);
    void toggle(PlayLayer* pl) { setEnabled(!m_enabled, pl); }

    Profile profile() const { return m_profile; }
    void setProfile(Profile p, PlayLayer* pl);
    void cycleProfile(PlayLayer* pl) { setProfile(nextProfile(m_profile), pl); }
    int maxFailures() const;

    // True while the solver itself is calling handleButton.
    bool sendingInput() const { return m_sending; }
    bool shouldBlockManualInput(PlayLayer* pl) const;

    void onLevelInit(PlayLayer* pl);
    void onLevelExit();
    void onBeforeReset(PlayLayer* pl);
    void onReset(PlayLayer* pl);
    void onTick(PlayLayer* pl);
    void onDeath(PlayLayer* pl, PlayerObject* player, GameObject* killer);
    void onPostUpdate(PlayLayer* pl);

    // Called once per rendered frame (from CCScheduler::update).
    void onFrame(PlayLayer* pl);
    // Game-speed multiplier for this frame (fast-forward through known parts).
    float timeMultiplier(PlayLayer* pl) const;
    // The game's own delayed respawn arrived after we already respawned.
    bool shouldSkipDelayedReset(PlayLayer* pl);
    bool inOwnReset() const { return m_inOwnReset; }

    bool active(PlayLayer* pl) const;
    std::string hudText(PlayLayer* pl) const;

private:
    // World
    void buildIndex(PlayLayer* pl);
    void refreshLive(PlayLayer* pl, uint64_t tick);
    void queryWindow(float x0, float x1, std::vector<WorldObject const*>& out) const;
    PlayerSnap snap(PlayerObject* p, uint8_t ch) const;
    Corridor corridor(PlayLayer* pl, uint8_t ch, PlayerSnap const& s, float lookahead) const;
    bool touchingOrb(PlayerSnap const& s) const;
    bool solidContact(PlayerSnap const& s) const;
    void updateBounds(uint8_t ch, PlayerSnap const& s);

    // Segments
    Segment& seg() { return m_segments.back(); }
    void syncSegments(PlayLayer* pl);
    Segment makeSegment(uint64_t startTick, bool hold0, bool hold1, bool ours);
    void onCheckpointAdded(PlayLayer* pl, uint64_t tick, bool ours);
    int channels(PlayLayer* pl) const;
    int spotFailures(uint64_t tick) const;

    // Input
    void press(PlayLayer* pl, uint8_t ch, bool down, uint64_t tick, bool record);
    void releaseAll(PlayLayer* pl);
    bool planStateAt(Segment const& s, uint8_t ch, uint64_t tick) const;

    // Live policies
    std::optional<bool> livePolicy(PlayLayer* pl, uint8_t ch, PlayerSnap const& s, uint64_t tick);
    std::optional<bool> flyController(PlayLayer* pl, uint8_t ch, PlayerSnap const& s, uint64_t tick);
    std::optional<bool> groundAssist(PlayerSnap const& s, uint8_t ch, uint64_t tick);
    FlyParams flyParamsAt(uint8_t ch, uint64_t tick) const;

    // Search
    void openCluster(Segment& s, uint64_t deathTick, uint8_t ch, Mode mode, int side, bool doomed, Trace const& trace,
                     std::vector<Event> const& attemptPlan);
    void rebuildBase(Segment& s, Cluster& c);
    void buildQueue(Segment& s, Cluster& c);
    void addPairs(Segment& s, Cluster& c, bool front);
    void addMicro(Segment& s, Cluster& c);
    void addSweep(Segment& s, Cluster& c);
    void reprioritize(Cluster& c);
    bool escalate(PlayLayer* pl, Segment& s, Cluster& c);
    void widenCluster(Segment& s, Cluster& c);
    void restartStuck(Segment& s);
    bool applyNextCandidate(Segment& s);
    void applyCandidate(Segment& s, Cluster& c, Candidate const& cand);
    void backtrack(PlayLayer* pl);
    uint64_t lookback(Mode m) const;
    uint64_t planHash(std::vector<Event> const& events, uint64_t uptoTick) const;
    uint64_t candidateKey(std::vector<Event> const& base, Candidate const& c) const;
    void describe(Candidate& c, uint64_t windowStart) const;
    bool respectsCooldown(std::vector<Event> const& base, std::vector<Event> const& cand, uint8_t ch) const;
    uint64_t nextCpInterval(uint64_t seed) const;

    // Polish pass
    void notePolish(Segment& s, Cluster& c);
    bool movePress(std::vector<Event>& events, uint8_t ch, uint64_t from, uint64_t to, int hold, int floorT,
                   bool check) const;
    void startPolish(Segment& s);
    bool resolvePolish(Segment& s, uint64_t deathTick); // true: the death was the polish's fault

    // Checkpoints
    bool safeForCheckpoint(PlayLayer* pl, PlayerSnap const& s, uint64_t tick) const;
    bool isDoomedState(PlayerSnap const& s) const;

    bool m_enabled = false;
    Profile m_profile = Profile::Human;
    bool m_sending = false;
    bool m_inLevel = false;

    std::vector<WorldObject> m_index;  // every relevant object, sorted by x (built once per level)
    std::vector<WorldObject> m_wide;   // very wide objects, scanned linearly
    float m_maxWidth = 0;
    std::vector<WorldObject> m_live;   // objects in the game's active sections, live positions
    std::unordered_set<GameObject*> m_liveSet;
    uint64_t m_liveTick = UINT64_MAX;

    std::vector<Segment> m_segments;
    std::vector<DoomedState> m_doomed;
    Trace m_trace;                    // current attempt
    Humanizer m_human;

    // Per-attempt runtime state
    bool m_hold[2] = {false, false};
    uint64_t m_lastToggle[2] = {0, 0};
    int64_t m_lastPress[2] = {-100000, -100000};
    int m_assistHold[2] = {0, 0};
    float m_vySmooth[2] = {0, 0};
    int64_t m_pendingToggleAt[2] = {-1, -1};
    bool m_pendingToggleTo[2] = {false, false};
    int64_t m_tapReleaseAt[2] = {-1, -1};
    uint64_t m_lastTick = UINT64_MAX;
    float m_prevY[2] = {0, 0};
    float m_prevXs[2] = {0, 0};
    bool m_havePrevY[2] = {false, false};
    Mode m_prevMode[2] = {Mode::Cube, Mode::Cube};
    bool m_modeKnown[2] = {false, false};
    float m_modeFloor[2] = {90.f, 90.f};
    float m_modeCeil[2] = {390.f, 390.f};
    std::map<int, float> m_learnedFloor[2]; // x bucket (100 units) -> floor y
    std::map<int, float> m_learnedCeil[2];  // x bucket (100 units) -> ceiling y
    bool m_expectOurs = false;
    bool m_deathHandled = false;
    bool m_divergedThisAttempt = false;
    uint64_t m_lastCheckpointTick = 0;
    uint64_t m_cpInterval = 240;
    uint64_t m_lastSpecialPass = 0;
    float m_prevX = 0;
    bool m_pendingCheckpoint = false;
    uint64_t m_assistJumpedFor = 0;
    bool m_fastForward = false;

    // Instant respawn
    bool m_respawnPending = false;
    int m_respawnFrames = 0;
    bool m_inOwnReset = false;
    bool m_fastRespawnBroken = false;
    uint64_t m_frame = 0;
    uint64_t m_lastFastResetFrame = 0;
    std::vector<uint64_t> m_skipTokens; // game respawn timers we already beat (tick counter at creation)
    uint64_t m_tickCounter = 0;         // monotonic count of physics ticks seen
    bool m_wasAliveBeforeReset = false;

    std::string m_status;
    int m_totalDeaths = 0;
};

} // namespace pf
