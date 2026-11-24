#pragma once

// Pathfinder: a trial-and-error search that runs inside Practice Mode.
//
// Every checkpoint starts a "segment". A segment owns an input plan: a list
// of press/release events at absolute physics ticks (m_currentProgress, which
// practice checkpoints save and restore, so replays line up tick-exactly).
//
//  * Up to `lockedUntil` the plan is replayed exactly.
//  * After that a live policy plays (nothing for ground modes apart from an
//    optional jump assist, a humanized corridor controller for flying modes)
//    and every toggle it makes is appended to the plan.
//  * On death, a "cluster" is opened at the death tick: the plan before a
//    look-back window is kept, and candidates for the window are tried one
//    by one in order of simplicity: no input, one click, longer holds, two
//    clicks, then micro-clicks once the spot failed more than 15 times.
//  * Safe progress places checkpoints automatically. A checkpoint with too
//    many failures is deleted and its parent segment is forced to change the
//    inputs that led into it (that prefix is blacklisted).
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
    Mode mode = Mode::Cube;
};

struct Trace {
    uint64_t base = 0;                 // tick of ticks[ch][0]
    std::vector<TraceTick> ticks[2];
    TraceTick const* at(uint8_t ch, uint64_t tick) const;
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
    bool ours = true; // placed by the solver (false: you placed it by hand)
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

    bool active(PlayLayer* pl) const;
    std::string hudText(PlayLayer* pl) const;

private:
    // World
    void buildIndex(PlayLayer* pl);
    void queryWindow(float x0, float x1, std::vector<WorldObject const*>& out) const;
    PlayerSnap snap(PlayerObject* p, uint8_t ch) const;
    Corridor corridor(PlayLayer* pl, uint8_t ch, PlayerSnap const& s, float lookahead) const;
    bool solidContact(PlayerSnap const& s) const;
    void updateBounds(uint8_t ch, PlayerSnap const& s);
    bool touchingOrb(PlayerSnap const& s) const;

    // Segments
    Segment& seg() { return m_segments.back(); }
    void syncSegments(PlayLayer* pl);
    Segment makeSegment(uint64_t startTick, bool hold0, bool hold1, bool ours);
    void onCheckpointAdded(PlayLayer* pl, uint64_t tick, bool ours);
    int spotFailures(uint64_t tick) const;
    void rebuildBase(Segment& s, Cluster& c);
    int channels(PlayLayer* pl) const;

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
    void buildQueue(Segment& s, Cluster& c);
    void addPairs(Segment& s, Cluster& c);
    void addMicro(Segment& s, Cluster& c);
    bool applyNextCandidate(Segment& s);
    void backtrack(PlayLayer* pl);
    uint64_t lookback(Mode m) const;
    uint64_t planHash(std::vector<Event> const& events, uint64_t uptoTick) const;
    uint64_t candidateKey(std::vector<Event> const& base, Candidate const& c) const;

    // Checkpoints
    bool safeForCheckpoint(PlayLayer* pl, PlayerSnap const& s, uint64_t tick) const;

    bool m_enabled = false;
    bool m_sending = false;
    bool m_inLevel = false;

    std::vector<WorldObject> m_index;
    std::vector<WorldObject> m_wide; // very wide objects, scanned linearly
    float m_maxWidth = 0;

    std::vector<Segment> m_segments;
    Trace m_trace;                    // current attempt
    Humanizer m_human;

    // Per-attempt runtime state
    bool m_hold[2] = {false, false};
    uint64_t m_lastToggle[2] = {0, 0};
    int64_t m_pendingToggleAt[2] = {-1, -1};
    bool m_pendingToggleTo[2] = {false, false};
    int64_t m_tapReleaseAt[2] = {-1, -1};
    uint64_t m_lastTick = UINT64_MAX;
    float m_prevY[2] = {0, 0};
    float m_prevXs[2] = {0, 0};
    bool m_havePrevY[2] = {false, false};
    Mode m_prevMode[2] = {Mode::Cube, Mode::Cube};
    bool m_modeKnown[2] = {false, false};
    std::map<int, float> m_learnedFloor[2]; // x bucket (100 units) -> floor y
    std::map<int, float> m_learnedCeil[2];  // x bucket (100 units) -> ceiling y
    bool m_expectOurs = false;
    float m_modeFloor[2] = {90.f, 90.f};
    float m_modeCeil[2] = {390.f, 390.f};
    bool m_deathHandled = false;
    uint64_t m_lastCheckpointTick = 0;
    uint64_t m_cpInterval = 240;
    uint64_t m_lastSpecialPass = 0;
    float m_prevX = 0;
    bool m_pendingCheckpoint = false;
    uint64_t m_assistJumpedFor = 0;
    std::string m_status;
    int m_totalDeaths = 0;
};

} // namespace pf
