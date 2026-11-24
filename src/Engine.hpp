#pragma once

// Macro Refiner engine.
//
// Everything runs inside the real game: inputs go to PlayerObject::pushButton /
// releaseButton on exact physics ticks, deaths come from PlayLayer::destroyPlayer
// and snapshots are the game's own practice checkpoints (CheckpointObject holds
// GJGameState, both PlayerCheckpoints and every object/trigger state), so GD's
// own physics, slopes, orbs and 2.2 triggers decide every outcome.
//
// Stages:
//   Log     one noclip run with the original inputs, logging player states per tick
//   1A      drop Cube/Robot/Spider presses made in mid-air that touched no orb
//   Merge   1B: holds re-pressed < 12 ticks after a release are merged if that
//           doesn't cause a death within 120 ticks
//   Repair  2: noclip off from the start; every death rolls back >= 60 ticks and
//           shifts the nearest press/release (or inserts a jump) until it survives
//   Polish  3: short holds get longer releases, jump starts move -3..+3 ticks to
//           the timing with the most room from hazards (deaths are repaired)
//   Verify  one clean noclip-off run from the start must reach 100%

#include <Geode/Geode.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "Macro.hpp"

namespace mr {

constexpr float kNoHazard = std::numeric_limits<float>::infinity();

enum class Stage { Idle, Log, Merge, Repair, Polish, Verify, Done, Failed };

enum class Job { None, Log, MergeBase, MergeMerged, MergeBaseline, Base, RepairTrial, ShiftTrial, ExtendTrial, Verify };

struct RunSpec {
    int64_t restoreTo = -1;      // latest snapshot with tick <= this; -1 = level start
    bool noclip = false;
    bool base = false;           // take a snapshot every 20 ticks
    bool log = false;            // per-tick state log (first pass)
    int64_t stopAt = std::numeric_limits<int64_t>::max();
    bool clearance = false;      // measure room from hazards
    int64_t clrFrom = 0, clrTo = -1;
    bool clrP2 = false;
    bool groundWatch = false;    // did a ground-mode player land during (gFrom, gTo]?
    int64_t gFrom = 0, gTo = -1;
    bool gP2 = false;
};

struct RunResult {
    bool died = false;
    int64_t deathTick = -1;
    bool deathP2 = false;
    bool completed = false;
    bool stuck = false;
    int64_t lastTick = -1;
    float minClear = kNoHazard;
    bool groundTouched = false;
};

// The player fields listed in the spec, saved next to every checkpoint so a
// restore can be checked against what was there.
struct PlayerState {
    cocos2d::CCPoint position;
    double yVelocity = 0.0;
    float rotation = 0.f;
    float playerSpeed = 0.f;
    float vehicleSize = 1.f;
    bool upsideDown = false, onGround = false, ship = false, ball = false, bird = false, dart = false,
         robot = false, spider = false, swing = false;
};

struct Snapshot {
    CheckpointObject* cp = nullptr;
    int64_t tick = -1;        // last processed tick when it was taken
    uint32_t counter = 0;     // m_currentProgress at that moment
    PlayerState p1, p2;
};

struct Indexed {
    GameObject* obj;
    float x;
};

class Engine {
public:
    static Engine& get();

    bool running() const { return m_stage != Stage::Idle && m_stage != Stage::Done && m_stage != Stage::Failed; }
    bool stepping() const { return m_stepping; }
    bool restoring() const { return m_restoring; }

    // Loads + checks the macro while the game is still paused. False = show err.
    bool prepare(PlayLayer* pl, std::string& err);
    void start(PlayLayer* pl);
    void stop(PlayLayer* pl);           // Stop button
    void onQuit(PlayLayer* pl);         // leaving the level

    void onFrame(PlayLayer* pl);        // once per rendered frame
    void onTick(PlayLayer* pl);         // after GJBaseGameLayer::processCommands
    void onDeath(PlayLayer* pl, PlayerObject* player);
    void onComplete(PlayLayer* pl);

    std::string statusText() const;

private:
    // runs
    void beginRun(PlayLayer* pl, Job job, RunSpec const& spec, std::vector<InputEvent> events);
    void finishRun(PlayLayer* pl);
    bool runShouldEnd() const;
    void step(PlayLayer* pl, int64_t ticks);
    void applyEvent(PlayLayer* pl, InputEvent const& ev);
    void resyncHeld(PlayLayer* pl);

    // snapshots
    void takeSnapshot(PlayLayer* pl);
    void restore(PlayLayer* pl, int64_t target);
    int64_t snapTickAtOrBefore(int64_t target) const;
    PlayerState capture(PlayerObject* p) const;

    // level knowledge
    void buildIndex(PlayLayer* pl);
    bool touchingOrb(PlayerObject* p) const;
    float hazardGap(PlayerObject* p) const;
    static bool groundMode(PlayerObject* p);
    void logState(PlayLayer* pl, int64_t t);

    // stages
    void startLog(PlayLayer* pl);
    void onLogDone(PlayLayer* pl);
    void stripMidAirTaps();
    void startMerge(PlayLayer* pl);
    void mergeNext(PlayLayer* pl, int64_t restoreTo);
    void onMergeBaseDone(PlayLayer* pl);
    void commitMerge(PlayLayer* pl, bool accept);
    void startRepairStage(PlayLayer* pl);
    void startPolish(PlayLayer* pl);
    void beginBase(PlayLayer* pl, int64_t restoreTo);
    void onBaseDone(PlayLayer* pl);
    void startFix(PlayLayer* pl, int64_t deathTick, bool p2);
    void buildFixCandidates();
    void tryNextFix(PlayLayer* pl);
    bool findPolishItem();
    void runPolishItem(PlayLayer* pl);
    void nextShiftTrial(PlayLayer* pl);
    void nextExtendTrial(PlayLayer* pl);
    void finishPolishItem(PlayLayer* pl, std::tuple<int64_t, int, int> key);
    void startVerify(PlayLayer* pl);
    void succeed(PlayLayer* pl);
    void fail(PlayLayer* pl, std::string const& why);
    void cleanup(PlayLayer* pl, bool levelAlive);

    // helpers
    bool twoPlayer(PlayLayer* pl) const;
    int channelOf(HoldSegment const& s) const { return (s.player2 ? 8 : 0) + s.button; }
    int prevSame(std::vector<HoldSegment> const& segs, size_t k) const;
    int nextSame(std::vector<HoldSegment> const& segs, size_t k) const;
    bool fitsChannel(std::vector<HoldSegment> const& segs, HoldSegment const& s, int ignore) const;
    float percentAt(int64_t tick) const;
    void setStage(Stage s);
    void createUI(PlayLayer* pl);
    void updateUI();

    Stage m_stage = Stage::Idle;
    Job m_job = Job::None;
    PlayLayer* m_pl = nullptr;

    Macro m_macro;
    std::vector<HoldSegment> m_segs;
    bool m_twoPlayer = false;

    // current run
    bool m_runActive = false;
    bool m_stepping = false;
    bool m_restoring = false;
    RunSpec m_spec;
    RunResult m_res;
    std::vector<InputEvent> m_runEvents;
    size_t m_cursor = 0;
    std::vector<int64_t> m_applied; // first pass: tick each original input fired on
    int64_t m_lastTick = -1;
    int64_t m_lastSnapTick = -1;
    int m_zeroSteps = 0;

    std::vector<Snapshot> m_snaps;

    // first-pass logs (bit 1 on ground, 2 ground mode, 4 touching orb, 8 dual)
    std::vector<uint8_t> m_logP1, m_logP2;
    std::vector<Indexed> m_hazards, m_rings;

    int64_t m_endTick = -1;
    int64_t m_tickCap = 0;
    int64_t m_progressTick = -1;

    // 1B
    struct MergeJob {
        size_t a = 0, b = 0;
        int64_t E = 0;
        int ch = 0;
        int64_t mergedDeath = -1;
        std::vector<HoldSegment> merged;
    } m_mj;
    std::tuple<int64_t, int> m_mergeKey{-1, -1};

    // 2
    struct FixJob {
        int64_t F = 0;
        bool p2 = false;
        int level = 0;
        int64_t target = -1;
        int64_t snapTick = -1;
        std::vector<std::vector<HoldSegment>> cands;
        size_t idx = 0;
        std::vector<HoldSegment> current;
    } m_fj;
    std::map<int64_t, int> m_fixSeen;
    int m_fixTrialsTotal = 0;

    // 3
    struct PolishItem {
        bool valid = false;
        int kind = 0; // 0 = shift start, 1 = extend release
        size_t seg = 0;
        int ch = 0;
        int64_t target = -1;
        std::tuple<int64_t, int, int> key{0, 0, 0};
    } m_pi;
    std::tuple<int64_t, int, int> m_polishKey{std::numeric_limits<int64_t>::min(), 0, 0};
    struct PolishTrials {
        std::vector<int> deltas;
        std::vector<std::vector<HoldSegment>> cands;
        std::vector<float> clear;
        std::vector<bool> died;
        size_t idx = 0;
    } m_pt;
    bool m_polishDone = false;
    int m_verifyFails = 0;

    // stats
    int m_removedTaps = 0, m_merges = 0, m_fixes = 0, m_shifted = 0, m_extended = 0;
    int m_trials = 0, m_restores = 0, m_counterMismatch = 0, m_restoreDrift = 0, m_snapFails = 0;
    std::chrono::steady_clock::time_point m_startTime;

    bool m_wasPractice = false;
    cocos2d::CCLabelBMFont* m_label = nullptr;
    cocos2d::CCMenu* m_menu = nullptr;
    std::string m_lastLabel;
};

} // namespace mr
