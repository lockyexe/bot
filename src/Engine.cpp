#include "Engine.hpp"

#include <algorithm>
#include <cmath>

using namespace geode::prelude;

namespace mr {

namespace {

constexpr int64_t kSnapEvery = 20;
constexpr int64_t kNever = std::numeric_limits<int64_t>::max();
constexpr int64_t kMergeGap = 12;     // 1B: re-press faster than this (50 ms) gets merged
constexpr int64_t kMergeWindow = 120; // 1B: survival window after the merge point
constexpr int64_t kFixAfter = 80;     // 2: must survive past death + 80
constexpr int64_t kShortHold = 14;    // 3: holds shorter than this get longer releases
char const* kBotName = "Macro Refiner";

template <class T>
T setting(char const* key) {
    return Mod::get()->getSettingValue<T>(key);
}

// Target object for the on-screen Stop button.
class StopTarget : public CCObject {
public:
    static StopTarget* shared() {
        static StopTarget* s = [] {
            auto t = new StopTarget();
            t->autorelease();
            t->retain();
            return t;
        }();
        return s;
    }
    void onStop(CCObject*) {
        if (auto pl = PlayLayer::get()) Engine::get().stop(pl);
    }
};

float rectGap(CCRect const& a, CCRect const& b) {
    float dx = std::max({0.f, a.getMinX() - b.getMaxX(), b.getMinX() - a.getMaxX()});
    float dy = std::max({0.f, a.getMinY() - b.getMaxY(), b.getMinY() - a.getMaxY()});
    return std::sqrt(dx * dx + dy * dy);
}

bool isRing(GameObjectType t) {
    switch (t) {
        case GameObjectType::YellowJumpRing:
        case GameObjectType::PinkJumpRing:
        case GameObjectType::GravityRing:
        case GameObjectType::GreenRing:
        case GameObjectType::DropRing:
        case GameObjectType::RedJumpRing:
        case GameObjectType::CustomRing:
        case GameObjectType::DashRing:
        case GameObjectType::GravityDashRing:
        case GameObjectType::SpiderOrb:
        case GameObjectType::TeleportOrb:
            return true;
        default:
            return false;
    }
}

} // namespace

Engine& Engine::get() {
    static Engine e;
    return e;
}

// ---------------------------------------------------------------------------
// setup
// ---------------------------------------------------------------------------

bool Engine::twoPlayer(PlayLayer* pl) const {
    return pl && pl->m_levelSettings && pl->m_levelSettings->m_twoPlayerMode;
}

bool Engine::prepare(PlayLayer* pl, std::string& err) {
    if (running()) {
        err = "The refiner is already running.";
        return false;
    }
    if (!pl || !pl->m_player1) {
        err = "Open a level first.";
        return false;
    }
    if (pl->m_isPlatformer || (pl->m_levelSettings && pl->m_levelSettings->m_platformerMode)) {
        err = "Platformer levels aren't supported (the refiner needs a level with a 100% end).";
        return false;
    }
    auto path = setting<std::filesystem::path>("macro-file");
    if (path.empty()) {
        err = "Pick your macro first: <cy>Mod settings</c> -> <cy>Macro file</c> (.gdr, .gdr.json or .gdr2).";
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        err = fmt::format("The macro file doesn't exist:\n<cy>{}</c>", path.string());
        return false;
    }
    Macro m;
    std::string lerr;
    if (!loadMacro(path, m, lerr)) {
        err = lerr;
        return false;
    }
    if (std::abs(m.framerate - 240.0) > 0.5) {
        err = fmt::format("This macro runs at {} TPS. Only normal 240 TPS macros (no physics bypass) are supported.", m.framerate);
        return false;
    }
    if (m.platformer) {
        err = "This is a platformer macro; only classic levels are supported.";
        return false;
    }
    bool hasJump = std::any_of(m.events.begin(), m.events.end(), [](InputEvent const& e) { return e.down && e.button == 1; });
    if (!hasJump) {
        err = "The macro has no jump inputs.";
        return false;
    }
    m_macro = std::move(m);
    return true;
}

void Engine::start(PlayLayer* pl) {
    if (running() || !pl) return;
    m_pl = pl;
    m_twoPlayer = twoPlayer(pl);
    if (!m_twoPlayer) {
        // One-player levels: every click is a player 1 click (dual mirrors it).
        for (auto& ev : m_macro.events) ev.player2 = false;
    }

    m_segs.clear();
    m_snaps.clear();
    m_logP1.clear();
    m_logP2.clear();
    m_fixSeen.clear();
    m_fixTrialsTotal = 0;
    m_endTick = -1;
    m_progressTick = -1;
    m_polishDone = false;
    m_verifyFails = 0;
    m_removedTaps = m_merges = m_fixes = m_shifted = m_extended = 0;
    m_trials = m_restores = m_counterMismatch = m_restoreDrift = m_snapFails = 0;
    m_mergeKey = {-1, -1};
    m_polishKey = {std::numeric_limits<int64_t>::min(), 0, 0};
    m_startTime = std::chrono::steady_clock::now();

    buildIndex(pl);

    // Practice mode: snapshots are practice checkpoints and a 100% here never
    // counts as a real completion.
    m_wasPractice = pl->m_isPracticeMode;
    if (!m_wasPractice) pl->togglePracticeMode(true);
    pl->removeAllCheckpoints();

    // The level only advances when we step it.
    CCDirector::get()->getScheduler()->pauseTarget(pl);
    FMODAudioEngine::sharedEngine()->stopAllMusic(true);

    createUI(pl);
    startLog(pl);
    log::info("Macro Refiner: started on {} ({} inputs, {})", pl->m_level ? std::string(pl->m_level->m_levelName) : "?",
              m_macro.events.size(), m_macro.path.filename().string());
}

void Engine::buildIndex(PlayLayer* pl) {
    m_hazards.clear();
    m_rings.clear();
    auto objs = pl->m_objects;
    if (!objs) return;
    for (unsigned i = 0; i < objs->count(); ++i) {
        auto obj = typeinfo_cast<GameObject*>(objs->objectAtIndex(i));
        if (!obj) continue;
        auto t = obj->m_objectType;
        if (t == GameObjectType::Hazard || t == GameObjectType::AnimatedHazard) {
            m_hazards.push_back({obj, obj->getPositionX()});
        } else if (isRing(t)) {
            m_rings.push_back({obj, obj->getPositionX()});
        }
    }
    auto byX = [](Indexed const& a, Indexed const& b) { return a.x < b.x; };
    std::sort(m_hazards.begin(), m_hazards.end(), byX);
    std::sort(m_rings.begin(), m_rings.end(), byX);
}

// ---------------------------------------------------------------------------
// running the game
// ---------------------------------------------------------------------------

void Engine::beginRun(PlayLayer* pl, Job job, RunSpec const& spec, std::vector<InputEvent> events) {
    m_job = job;
    m_spec = spec;
    m_res = RunResult{};
    m_runEvents = std::move(events);
    restore(pl, spec.restoreTo);
    m_cursor = 0;
    if (m_lastTick >= 0) {
        while (m_cursor < m_runEvents.size() && static_cast<int64_t>(m_runEvents[m_cursor].frame) <= m_lastTick) ++m_cursor;
    }
    resyncHeld(pl);
    m_zeroSteps = 0;
    m_runActive = true;
    ++m_trials;
}

void Engine::resyncHeld(PlayLayer* pl) {
    if (m_lastTick < 0) return;
    // Which channels are held at the restore point, per the inputs being played.
    bool held[2][4] = {};
    for (auto const& ev : m_runEvents) {
        if (static_cast<int64_t>(ev.frame) > m_lastTick) break;
        if (ev.button < 1 || ev.button > 3) continue;
        held[ev.player2 ? 1 : 0][ev.button] = ev.down;
    }
    for (int p = 0; p < 2; ++p) {
        for (int b = 1; b <= 3; ++b) {
            if (!held[p][b]) continue;
            InputEvent up{0, b, p == 1, false};
            InputEvent down{0, b, p == 1, true};
            applyEvent(pl, up);
            applyEvent(pl, down);
        }
    }
}

void Engine::applyEvent(PlayLayer* pl, InputEvent const& ev) {
    auto button = static_cast<PlayerButton>(ev.button);
    auto act = [&](PlayerObject* p) {
        if (!p) return;
        if (ev.down) p->pushButton(button);
        else p->releaseButton(button);
    };
    if (m_twoPlayer) {
        act(ev.player2 ? pl->m_player2 : pl->m_player1);
    } else {
        act(pl->m_player1);
        if (pl->m_gameState.m_isDualMode) act(pl->m_player2);
    }
}

void Engine::step(PlayLayer* pl, int64_t ticks) {
    float warp = pl->m_gameState.m_timeWarp;
    if (!(warp > 0.f) || warp > 1.f) warp = 1.f;
    double dt = static_cast<double>(ticks) * (static_cast<double>(warp) / 240.0);
    m_stepping = true;
    pl->update(static_cast<float>(dt));
    m_stepping = false;
}

bool Engine::runShouldEnd() const {
    if (!m_runActive) return false;
    if (m_res.died && !m_spec.noclip) return true;
    if (m_res.completed || m_res.stuck) return true;
    return m_lastTick >= m_spec.stopAt;
}

void Engine::onFrame(PlayLayer* pl) {
    if (!running() || pl != m_pl) return;
    auto t0 = std::chrono::steady_clock::now();
    int64_t budget = std::max<int64_t>(1, setting<int64_t>("ticks-per-frame"));
    int64_t maxMs = std::max<int64_t>(4, setting<int64_t>("max-ms-per-frame"));
    int guard = 0;

    while (running() && budget > 0 && guard++ < 5000) {
        if (!m_runActive) break;
        auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        if (spent >= maxMs) break;
        if (runShouldEnd()) {
            finishRun(pl);
            continue;
        }
        int64_t n = std::min<int64_t>(budget, kSnapEvery);
        if (m_spec.stopAt != kNever) n = std::min<int64_t>(n, m_spec.stopAt - m_lastTick);
        if (m_spec.base) n = std::min<int64_t>(n, m_lastSnapTick + kSnapEvery - m_lastTick);
        n = std::max<int64_t>(n, 1);

        int64_t before = m_lastTick;
        step(pl, n);
        int64_t moved = m_lastTick - before;
        if (moved <= 0) {
            budget -= 1;
            if (++m_zeroSteps > 240) m_res.stuck = true;
        } else {
            budget -= moved;
            m_zeroSteps = 0;
        }
        if (m_spec.base && !m_res.died && m_lastTick >= m_lastSnapTick + kSnapEvery) takeSnapshot(pl);
        if (m_lastTick > m_tickCap) m_res.stuck = true;
        if (m_spec.base || m_job == Job::Log || m_job == Job::Verify) m_progressTick = std::max(m_progressTick, m_lastTick);
        if (runShouldEnd()) finishRun(pl);
    }
    updateUI();
}

void Engine::onTick(PlayLayer* pl) {
    if (!m_runActive || !m_stepping || pl != m_pl) return;
    int64_t t = pl->m_gameState.m_currentProgress;
    m_lastTick = t;

    if (m_job == Job::Log && m_macro.xdBotFrames) {
        // xdBot's own macros: frame = int(levelTime * 240) + 1.
        int64_t xf = static_cast<int64_t>(pl->m_gameState.m_levelTime * m_macro.framerate) + 1;
        while (m_cursor < m_runEvents.size() && static_cast<int64_t>(m_runEvents[m_cursor].frame) + m_macro.xdOffset <= xf) {
            applyEvent(pl, m_runEvents[m_cursor]);
            m_applied[m_cursor] = t;
            ++m_cursor;
        }
    } else {
        while (m_cursor < m_runEvents.size() && static_cast<int64_t>(m_runEvents[m_cursor].frame) <= t) {
            applyEvent(pl, m_runEvents[m_cursor]);
            if (m_job == Job::Log) m_applied[m_cursor] = t;
            ++m_cursor;
        }
    }

    if (m_spec.log) logState(pl, t);

    // Fallback in case the level ends some way the completion hooks don't see.
    if (!pl->m_isPlatformer && pl->m_player1 && pl->m_levelLength > 0.f &&
        pl->m_player1->getPositionX() > pl->m_levelLength + 300.f) {
        m_res.completed = true;
    }

    if (m_spec.clearance && t >= m_spec.clrFrom && t <= m_spec.clrTo) {
        auto check = [&](PlayerObject* p) {
            if (p) m_res.minClear = std::min(m_res.minClear, hazardGap(p));
        };
        if (m_twoPlayer) {
            check(m_spec.clrP2 ? pl->m_player2 : pl->m_player1);
        } else {
            check(pl->m_player1);
            if (pl->m_gameState.m_isDualMode) check(pl->m_player2);
        }
    }

    if (m_spec.groundWatch && t > m_spec.gFrom && t <= m_spec.gTo) {
        auto check = [&](PlayerObject* p) {
            if (p && p->m_isOnGround && groundMode(p)) m_res.groundTouched = true;
        };
        if (m_twoPlayer) {
            check(m_spec.gP2 ? pl->m_player2 : pl->m_player1);
        } else {
            check(pl->m_player1);
            if (pl->m_gameState.m_isDualMode) check(pl->m_player2);
        }
    }
}

void Engine::onDeath(PlayLayer* pl, PlayerObject* player) {
    if (!m_runActive || m_spec.noclip || m_res.died) return;
    m_res.died = true;
    m_res.deathTick = pl->m_gameState.m_currentProgress;
    m_res.deathP2 = player && player == pl->m_player2;
}

void Engine::onComplete(PlayLayer* pl) {
    if (!m_runActive) return;
    m_res.completed = true;
    (void)pl;
}

// ---------------------------------------------------------------------------
// snapshots (practice checkpoints)
// ---------------------------------------------------------------------------

PlayerState Engine::capture(PlayerObject* p) const {
    PlayerState s;
    if (!p) return s;
    s.position = p->m_position;
    s.yVelocity = p->m_yVelocity;
    s.rotation = p->getRotation();
    s.playerSpeed = p->m_playerSpeed;
    s.vehicleSize = p->m_vehicleSize;
    s.upsideDown = p->m_isUpsideDown;
    s.onGround = p->m_isOnGround;
    s.ship = p->m_isShip;
    s.ball = p->m_isBall;
    s.bird = p->m_isBird;
    s.dart = p->m_isDart;
    s.robot = p->m_isRobot;
    s.spider = p->m_isSpider;
    s.swing = p->m_isSwing;
    return s;
}

void Engine::takeSnapshot(PlayLayer* pl) {
    m_lastSnapTick = m_lastTick;
    auto arr = pl->m_checkpointArray;
    if (!arr) {
        ++m_snapFails;
        return;
    }
    unsigned before = arr->count();
    pl->markCheckpoint();
    if (arr->count() != before + 1) {
        ++m_snapFails;
        return;
    }
    auto cp = static_cast<CheckpointObject*>(arr->lastObject());
    if (cp->m_physicalCheckpointObject) cp->m_physicalCheckpointObject->setVisible(false);
    Snapshot s;
    s.cp = cp;
    s.tick = m_lastTick;
    s.counter = pl->m_gameState.m_currentProgress;
    s.p1 = capture(pl->m_player1);
    s.p2 = capture(pl->m_player2);
    m_snaps.push_back(s);

    // Ring buffer: drop the oldest.
    size_t ring = static_cast<size_t>(std::max<int64_t>(8, setting<int64_t>("snapshot-ring")));
    while (m_snaps.size() > ring && arr->count() > 0) {
        auto first = arr->objectAtIndex(0);
        auto it = std::find_if(m_snaps.begin(), m_snaps.end(), [&](Snapshot const& x) { return x.cp == first; });
        if (it != m_snaps.end()) m_snaps.erase(it);
        unsigned c = arr->count();
        pl->removeCheckpoint(true);
        if (arr->count() == c) break;
    }
}

int64_t Engine::snapTickAtOrBefore(int64_t target) const {
    int64_t best = -1;
    for (auto const& s : m_snaps) {
        if (s.tick <= target) best = std::max(best, s.tick);
    }
    return best;
}

void Engine::restore(PlayLayer* pl, int64_t target) {
    auto arr = pl->m_checkpointArray;
    // Pop every checkpoint newer than the target (and anything that isn't ours).
    while (arr && arr->count() > 0) {
        auto last = arr->lastObject();
        auto it = std::find_if(m_snaps.begin(), m_snaps.end(), [&](Snapshot const& x) { return x.cp == last; });
        if (it != m_snaps.end() && it->tick <= target) break;
        if (it != m_snaps.end()) m_snaps.erase(it);
        unsigned c = arr->count();
        pl->removeCheckpoint(false);
        if (arr->count() == c) arr->removeLastObject();
    }
    Snapshot const* snap = nullptr;
    if (arr && arr->count() > 0) {
        auto last = arr->lastObject();
        for (auto const& s : m_snaps) {
            if (s.cp == last) snap = &s;
        }
    }

    m_restoring = true;
    m_stepping = false;
    pl->resetLevel(); // practice mode: loads the last checkpoint, or the start if none
    m_restoring = false;
    ++m_restores;

    if (snap) {
        m_lastTick = snap->tick;
        m_lastSnapTick = snap->tick;
        if (pl->m_gameState.m_currentProgress != snap->counter) ++m_counterMismatch;
        if (pl->m_player1) {
            auto now = pl->m_player1->m_position;
            if (std::abs(now.x - snap->p1.position.x) > 0.01f || std::abs(now.y - snap->p1.position.y) > 0.01f) ++m_restoreDrift;
        }
    } else {
        m_lastTick = -1;
        m_lastSnapTick = -1;
    }
}

// ---------------------------------------------------------------------------
// level knowledge
// ---------------------------------------------------------------------------

bool Engine::groundMode(PlayerObject* p) {
    return p && !p->m_isShip && !p->m_isBall && !p->m_isBird && !p->m_isDart && !p->m_isSwing;
}

bool Engine::touchingOrb(PlayerObject* p) const {
    if (!p) return false;
    if (p->m_touchingRings && p->m_touchingRings->count() > 0) return true;
    CCRect r = p->getObjectRect();
    float px = r.getMidX();
    auto lo = std::lower_bound(m_rings.begin(), m_rings.end(), px - 90.f, [](Indexed const& a, float x) { return a.x < x; });
    for (auto it = lo; it != m_rings.end() && it->x <= px + 90.f; ++it) {
        if (r.intersectsRect(it->obj->getObjectRect())) return true;
    }
    return false;
}

float Engine::hazardGap(PlayerObject* p) const {
    CCRect r = p->getObjectRect();
    float px = r.getMidX();
    float best = kNoHazard;
    auto lo = std::lower_bound(m_hazards.begin(), m_hazards.end(), px - 120.f, [](Indexed const& a, float x) { return a.x < x; });
    for (auto it = lo; it != m_hazards.end() && it->x <= px + 120.f; ++it) {
        auto obj = it->obj;
        if (!obj->isVisible()) continue;
        best = std::min(best, rectGap(r, obj->getObjectRect()));
    }
    return best;
}

void Engine::logState(PlayLayer* pl, int64_t t) {
    if (t < 0 || t > 50'000'000) return;
    auto idx = static_cast<size_t>(t);
    if (m_logP1.size() <= idx) {
        m_logP1.resize(idx + 1, 0);
        m_logP2.resize(idx + 1, 0);
    }
    auto flags = [&](PlayerObject* p) -> uint8_t {
        if (!p) return 0;
        uint8_t f = 0;
        if (p->m_isOnGround) f |= 1;
        if (groundMode(p)) f |= 2;
        if (touchingOrb(p)) f |= 4;
        return f;
    };
    m_logP1[idx] = flags(pl->m_player1) | (pl->m_gameState.m_isDualMode ? 8 : 0);
    m_logP2[idx] = flags(pl->m_player2);
}

// ---------------------------------------------------------------------------
// segment helpers
// ---------------------------------------------------------------------------

int Engine::prevSame(std::vector<HoldSegment> const& segs, size_t k) const {
    for (size_t i = k; i-- > 0;) {
        if (sameChannel(segs[i], segs[k])) return static_cast<int>(i);
    }
    return -1;
}

int Engine::nextSame(std::vector<HoldSegment> const& segs, size_t k) const {
    for (size_t i = k + 1; i < segs.size(); ++i) {
        if (sameChannel(segs[i], segs[k])) return static_cast<int>(i);
    }
    return -1;
}

// Does s fit in its channel without touching another hold (ignoring index `ignore`)?
bool Engine::fitsChannel(std::vector<HoldSegment> const& segs, HoldSegment const& s, int ignore) const {
    for (size_t i = 0; i < segs.size(); ++i) {
        if (static_cast<int>(i) == ignore || !sameChannel(segs[i], s)) continue;
        auto const& o = segs[i];
        uint64_t oEnd = o.endFrame == kOpenEnd ? UINT64_MAX : o.endFrame;
        uint64_t sEnd = s.endFrame == kOpenEnd ? UINT64_MAX : s.endFrame;
        bool before = sEnd < o.startFrame;
        bool after = s.startFrame > oEnd;
        if (!before && !after) return false;
    }
    return true;
}

float Engine::percentAt(int64_t tick) const {
    if (m_endTick <= 0) return 0.f;
    return std::clamp(100.f * static_cast<float>(tick) / static_cast<float>(m_endTick), 0.f, 100.f);
}

void Engine::setStage(Stage s) {
    m_stage = s;
    m_progressTick = -1;
}

// ---------------------------------------------------------------------------
// Log pass + Step 1A
// ---------------------------------------------------------------------------

void Engine::startLog(PlayLayer* pl) {
    setStage(Stage::Log);
    RunSpec s;
    s.restoreTo = -1;
    s.noclip = true;
    s.log = true;
    int64_t lastFrame = m_macro.events.empty() ? 0 : m_macro.events.back().frame;
    m_tickCap = std::max<int64_t>(lastFrame + 240 * 60, 240 * 60 * 3);
    m_applied.assign(m_macro.events.size(), -1);
    beginRun(pl, Job::Log, s, m_macro.events);
}

void Engine::onLogDone(PlayLayer* pl) {
    if (!m_res.completed) {
        float pct = 0.f;
        if (pl->m_player1 && pl->m_levelLength > 0) pct = std::clamp(100.f * pl->m_player1->getPositionX() / pl->m_levelLength, 0.f, 100.f);
        fail(pl, fmt::format(
            "With noclip on, the macro doesn't finish this level (stopped around {:.0f}%).\n"
            "Make sure it's the macro for this level, recorded from the start (no start position).", pct));
        return;
    }
    m_endTick = m_res.lastTick;
    m_tickCap = m_endTick + 240 * 10;

    std::vector<InputEvent> evs;
    evs.reserve(m_macro.events.size());
    for (size_t i = 0; i < m_macro.events.size(); ++i) {
        int64_t at = m_applied[i];
        if (at < 0 || at > m_endTick) continue; // never reached / after the end
        InputEvent ev = m_macro.events[i];
        ev.frame = static_cast<uint32_t>(at);
        evs.push_back(ev);
    }
    std::stable_sort(evs.begin(), evs.end(), [](InputEvent const& a, InputEvent const& b) { return a.frame < b.frame; });
    m_segs = toSegments(evs);
    stripMidAirTaps();
    log::info("Macro Refiner: level ends at tick {}, {} holds, removed {} mid-air taps", m_endTick, m_segs.size(), m_removedTaps);
    startMerge(pl);
}

void Engine::stripMidAirTaps() {
    auto at = [&](std::vector<uint8_t> const& v, int64_t t) -> uint8_t {
        if (t < 0 || static_cast<size_t>(t) >= v.size()) return 0;
        return v[static_cast<size_t>(t)];
    };
    std::vector<HoldSegment> kept;
    kept.reserve(m_segs.size());
    for (auto const& s : m_segs) {
        if (s.button != 1) {
            kept.push_back(s);
            continue;
        }
        int64_t t = s.startFrame;
        std::vector<std::vector<uint8_t> const*> logs;
        if (m_twoPlayer) {
            logs.push_back(s.player2 ? &m_logP2 : &m_logP1);
        } else {
            logs.push_back(&m_logP1);
            if (at(m_logP1, t) & 8) logs.push_back(&m_logP2);
        }
        bool useless = true;
        for (auto log : logs) {
            uint8_t f = at(*log, t);
            bool ground = f & 1;
            bool gmode = f & 2;
            if (!gmode || ground) { useless = false; break; }
            bool orb = false;
            for (int64_t k = t - 3; k <= t + 3; ++k) {
                if (at(*log, k) & 4) { orb = true; break; }
            }
            if (orb) { useless = false; break; }
            // Held into the landing = a buffered jump, which a real player does too.
            int64_t end = s.endFrame == kOpenEnd ? m_endTick : std::min<int64_t>(s.endFrame, m_endTick);
            bool lands = false;
            for (int64_t k = t + 1; k <= end; ++k) {
                uint8_t g = at(*log, k);
                if ((g & 1) && (g & 2)) { lands = true; break; }
            }
            if (lands) { useless = false; break; }
        }
        if (useless) ++m_removedTaps;
        else kept.push_back(s);
    }
    m_segs = std::move(kept);
}

// ---------------------------------------------------------------------------
// Step 1B: merge inhuman re-presses
// ---------------------------------------------------------------------------

void Engine::startMerge(PlayLayer* pl) {
    setStage(Stage::Merge);
    m_mergeKey = {-1, -1};
    mergeNext(pl, -1);
}

void Engine::mergeNext(PlayLayer* pl, int64_t restoreTo) {
    bool found = false;
    std::tuple<int64_t, int> best{kNever, 0};
    for (size_t i = 0; i < m_segs.size(); ++i) {
        auto const& s = m_segs[i];
        if (s.button != 1 || s.endFrame == kOpenEnd || static_cast<int64_t>(s.endFrame) >= m_endTick) continue;
        int j = nextSame(m_segs, i);
        if (j < 0) continue;
        int64_t gap = static_cast<int64_t>(m_segs[j].startFrame) - static_cast<int64_t>(s.endFrame);
        if (gap < 0 || gap >= kMergeGap) continue;
        std::tuple<int64_t, int> key{s.endFrame, channelOf(s)};
        if (key > m_mergeKey && key < best) {
            best = key;
            m_mj.a = i;
            m_mj.b = static_cast<size_t>(j);
            m_mj.E = s.endFrame;
            m_mj.ch = channelOf(s);
            found = true;
        }
    }
    if (!found) {
        startRepairStage(pl);
        return;
    }
    RunSpec s;
    s.restoreTo = restoreTo;
    s.noclip = true;
    s.base = true;
    s.stopAt = m_mj.E - 1;
    beginRun(pl, Job::MergeBase, s, toEvents(m_segs));
}

void Engine::onMergeBaseDone(PlayLayer* pl) {
    if (m_res.stuck) {
        fail(pl, "The game stopped advancing while merging clicks.");
        return;
    }
    if (m_lastTick < m_mj.E - 1) {
        // Level ended before this point.
        startRepairStage(pl);
        return;
    }
    m_mj.merged = m_segs;
    m_mj.merged[m_mj.a].endFrame = m_mj.merged[m_mj.b].endFrame;
    m_mj.merged.erase(m_mj.merged.begin() + static_cast<std::ptrdiff_t>(m_mj.b));
    m_mj.mergedDeath = -1;

    RunSpec s;
    s.restoreTo = m_mj.E - 1;
    s.noclip = false;
    s.stopAt = m_mj.E + kMergeWindow;
    beginRun(pl, Job::MergeMerged, s, toEvents(m_mj.merged));
}

void Engine::commitMerge(PlayLayer* pl, bool accept) {
    m_mergeKey = {m_mj.E, m_mj.ch};
    if (accept) {
        m_segs = m_mj.merged;
        ++m_merges;
    }
    mergeNext(pl, m_mj.E - 1);
}

// ---------------------------------------------------------------------------
// Step 2 (+ the sweep Step 3 runs on): noclip off, deaths get repaired
// ---------------------------------------------------------------------------

void Engine::startRepairStage(PlayLayer* pl) {
    setStage(Stage::Repair);
    beginBase(pl, -1);
}

void Engine::startPolish(PlayLayer* pl) {
    setStage(Stage::Polish);
    m_polishKey = {std::numeric_limits<int64_t>::min(), 0, 0};
    beginBase(pl, -1);
}

void Engine::beginBase(PlayLayer* pl, int64_t restoreTo) {
    RunSpec s;
    s.restoreTo = restoreTo;
    s.noclip = false;
    s.base = true;
    if (m_stage == Stage::Polish && findPolishItem()) s.stopAt = m_pi.target;
    else m_pi.valid = false;
    beginRun(pl, Job::Base, s, toEvents(m_segs));
}

void Engine::onBaseDone(PlayLayer* pl) {
    if (m_res.died) {
        startFix(pl, m_res.deathTick, m_res.deathP2);
        return;
    }
    if (m_res.completed) {
        if (m_stage == Stage::Polish) {
            m_polishDone = true;
            startVerify(pl);
        } else if (m_polishDone) {
            startVerify(pl);
        } else {
            startPolish(pl);
        }
        return;
    }
    if (m_res.stuck) {
        fail(pl, fmt::format("The run got stuck around {:.0f}% without finishing the level.", percentAt(m_lastTick)));
        return;
    }
    if (m_stage == Stage::Polish && m_pi.valid) {
        runPolishItem(pl);
        return;
    }
    fail(pl, "Internal error: the sweep stopped for no reason.");
}

void Engine::startFix(PlayLayer* pl, int64_t deathTick, bool p2) {
    int seen = ++m_fixSeen[deathTick];
    if (seen > 4) {
        fail(pl, fmt::format("The death at tick {} ({:.0f}%) keeps coming back after fixes.", deathTick, percentAt(deathTick)));
        return;
    }
    m_fj = FixJob{};
    m_fj.F = deathTick;
    m_fj.p2 = p2;
    m_fj.level = std::min(2, seen - 1);
    buildFixCandidates();
    tryNextFix(pl);
}

void Engine::buildFixCandidates() {
    static int64_t const backs[3] = {60, 140, 240};
    int64_t F = m_fj.F;
    m_fj.target = F - backs[m_fj.level];
    m_fj.snapTick = snapTickAtOrBefore(m_fj.target);
    m_fj.cands.clear();
    m_fj.idx = 0;

    int64_t lo = m_fj.snapTick + 1; // first tick that can still be changed
    bool p2 = m_twoPlayer && m_fj.p2;
    auto inChannel = [&](HoldSegment const& s) { return s.button == 1 && s.player2 == p2; };

    struct Point {
        size_t seg;
        bool end;
        int64_t frame;
    };
    std::vector<Point> pts;
    for (size_t k = 0; k < m_segs.size(); ++k) {
        auto const& s = m_segs[k];
        if (!inChannel(s)) continue;
        if (s.startFrame >= lo && s.startFrame <= F) pts.push_back({k, false, s.startFrame});
        if (s.endFrame != kOpenEnd && static_cast<int64_t>(s.endFrame) >= lo && static_cast<int64_t>(s.endFrame) <= F) {
            pts.push_back({k, true, s.endFrame});
        }
    }
    std::stable_sort(pts.begin(), pts.end(), [&](Point const& a, Point const& b) { return F - a.frame < F - b.frame; });

    static int const deltas[] = {-2, 2, -4, 4, -6, 6, -8, 8, -10, 10, -12, 12};
    for (auto const& pt : pts) {
        auto const& s = m_segs[pt.seg];
        int pv = prevSame(m_segs, pt.seg);
        int nx = nextSame(m_segs, pt.seg);
        for (int d : deltas) {
            int64_t nf = pt.frame + d;
            if (nf < lo || nf < 0) continue;
            if (!pt.end) {
                if (pv >= 0 && nf <= static_cast<int64_t>(m_segs[pv].endFrame)) continue;
                if (s.endFrame != kOpenEnd && nf > static_cast<int64_t>(s.endFrame)) continue;
            } else {
                if (nf < static_cast<int64_t>(s.startFrame)) continue;
                if (nx >= 0 && nf >= static_cast<int64_t>(m_segs[nx].startFrame)) continue;
            }
            auto c = m_segs;
            if (pt.end) c[pt.seg].endFrame = static_cast<uint32_t>(nf);
            else c[pt.seg].startFrame = static_cast<uint32_t>(nf);
            sortSegments(c);
            m_fj.cands.push_back(std::move(c));
        }
    }

    auto tryInsert = [&](int64_t start, int64_t end) {
        if (start < lo || start < 0 || end <= start) return;
        HoldSegment h;
        h.startFrame = static_cast<uint32_t>(start);
        h.endFrame = static_cast<uint32_t>(end);
        h.button = 1;
        h.player2 = p2;
        if (!fitsChannel(m_segs, h, -1)) return;
        auto c = m_segs;
        c.push_back(h);
        sortSegments(c);
        m_fj.cands.push_back(std::move(c));
    };

    bool recentInput = std::any_of(pts.begin(), pts.end(), [&](Point const& p) { return p.frame >= F - 60; });
    if (!recentInput || m_fj.level >= 1) tryInsert(F - 24, F - 8); // a 16-tick jump just before the death
    if (m_fj.level >= 1) {
        for (int64_t back : {40, 32, 28, 20, 16, 12, 6}) {
            for (int64_t len : {8, 16, 24}) tryInsert(F - back, F - back + len);
        }
        for (size_t k = 0; k < m_segs.size(); ++k) {
            auto const& s = m_segs[k];
            if (!inChannel(s) || static_cast<int64_t>(s.startFrame) < lo || static_cast<int64_t>(s.startFrame) > F) continue;
            auto c = m_segs;
            c.erase(c.begin() + static_cast<std::ptrdiff_t>(k));
            m_fj.cands.push_back(std::move(c));
        }
    }
    if (m_fj.level >= 2) {
        for (int64_t back : {80, 64, 52}) {
            for (int64_t len : {8, 16, 24}) tryInsert(F - back, F - back + len);
        }
    }
}

void Engine::tryNextFix(PlayLayer* pl) {
    if (++m_fixTrialsTotal > 20000) {
        fail(pl, "Too many repair attempts; giving up.");
        return;
    }
    if (m_fj.idx < m_fj.cands.size()) {
        m_fj.current = m_fj.cands[m_fj.idx++];
        RunSpec s;
        s.restoreTo = m_fj.target;
        s.noclip = false;
        s.stopAt = m_fj.F + kFixAfter;
        beginRun(pl, Job::RepairTrial, s, toEvents(m_fj.current));
        return;
    }
    if (m_fj.level < 2) {
        ++m_fj.level;
        buildFixCandidates();
        tryNextFix(pl);
        return;
    }
    fail(pl, fmt::format("Couldn't fix the death at tick {} ({:.0f}%) by moving, adding or removing nearby clicks.",
                         m_fj.F, percentAt(m_fj.F)));
}

// ---------------------------------------------------------------------------
// Step 3: humanized polish
// ---------------------------------------------------------------------------

bool Engine::findPolishItem() {
    bool found = false;
    std::tuple<int64_t, int, int> best{kNever, 0, 0};
    for (size_t k = 0; k < m_segs.size(); ++k) {
        auto const& s = m_segs[k];
        if (s.button != 1) continue;
        int ch = channelOf(s);
        if (s.startFrame >= 4 && static_cast<int64_t>(s.startFrame) < m_endTick) {
            std::tuple<int64_t, int, int> key{static_cast<int64_t>(s.startFrame) - 3, 0, ch};
            if (key > m_polishKey && key < best) {
                best = key;
                m_pi = PolishItem{true, 0, k, ch, static_cast<int64_t>(s.startFrame) - 4, key};
                found = true;
            }
        }
        if (s.endFrame != kOpenEnd && s.endFrame - s.startFrame < kShortHold && static_cast<int64_t>(s.endFrame) < m_endTick) {
            std::tuple<int64_t, int, int> key{static_cast<int64_t>(s.endFrame), 1, ch};
            if (key > m_polishKey && key < best) {
                best = key;
                m_pi = PolishItem{true, 1, k, ch, static_cast<int64_t>(s.endFrame) - 1, key};
                found = true;
            }
        }
    }
    if (!found) m_pi.valid = false;
    return found;
}

void Engine::runPolishItem(PlayLayer* pl) {
    m_pt = PolishTrials{};
    auto const& s = m_segs[m_pi.seg];
    if (m_pi.kind == 0) {
        int pv = prevSame(m_segs, m_pi.seg);
        for (int d : {0, -1, 1, -2, 2, -3, 3}) {
            int64_t ns = static_cast<int64_t>(s.startFrame) + d;
            if (ns <= m_pi.target) continue;
            if (pv >= 0 && ns <= static_cast<int64_t>(m_segs[pv].endFrame)) continue;
            if (s.endFrame != kOpenEnd && ns > static_cast<int64_t>(s.endFrame)) continue;
            auto c = m_segs;
            c[m_pi.seg].startFrame = static_cast<uint32_t>(ns);
            sortSegments(c);
            m_pt.deltas.push_back(d);
            m_pt.cands.push_back(std::move(c));
        }
        if (m_pt.deltas.empty() || m_pt.deltas.front() != 0) {
            finishPolishItem(pl, m_pi.key); // no room to move it
            return;
        }
        nextShiftTrial(pl);
    } else {
        int nx = nextSame(m_segs, m_pi.seg);
        for (int k : {20, 16, 12, 8, 4}) {
            int64_t ne = static_cast<int64_t>(s.endFrame) + k;
            if (ne >= m_endTick) continue;
            if (nx >= 0 && ne >= static_cast<int64_t>(m_segs[nx].startFrame) - 3) continue;
            auto c = m_segs;
            c[m_pi.seg].endFrame = static_cast<uint32_t>(ne);
            m_pt.deltas.push_back(k);
            m_pt.cands.push_back(std::move(c));
        }
        nextExtendTrial(pl);
    }
}

void Engine::nextShiftTrial(PlayLayer* pl) {
    auto const& orig = m_segs[m_pi.seg];
    if (m_pt.idx >= m_pt.cands.size()) {
        // Pick the start with the most room from hazards.
        int bestIdx = -1;
        float baseClear = kNoHazard;
        for (size_t i = 0; i < m_pt.clear.size(); ++i) {
            if (m_pt.deltas[i] == 0) baseClear = m_pt.clear[i];
        }
        for (size_t i = 0; i < m_pt.clear.size(); ++i) {
            if (m_pt.died[i]) continue;
            if (bestIdx < 0 || m_pt.clear[i] > m_pt.clear[bestIdx] + 1e-3f ||
                (std::abs(m_pt.clear[i] - m_pt.clear[bestIdx]) <= 1e-3f && std::abs(m_pt.deltas[i]) < std::abs(m_pt.deltas[bestIdx]))) {
                bestIdx = static_cast<int>(i);
            }
        }
        auto key = m_pi.key;
        if (bestIdx >= 0 && m_pt.deltas[bestIdx] != 0 && m_pt.clear[bestIdx] > baseClear + 0.5f) {
            int64_t ns = static_cast<int64_t>(orig.startFrame) + m_pt.deltas[bestIdx];
            m_segs = m_pt.cands[bestIdx];
            ++m_shifted;
            key = std::max(key, std::tuple<int64_t, int, int>{ns - 3, 0, m_pi.ch});
        }
        finishPolishItem(pl, key);
        return;
    }
    int d = m_pt.deltas[m_pt.idx];
    int64_t ns = static_cast<int64_t>(orig.startFrame) + d;
    auto events = toEvents(m_pt.cands[m_pt.idx]);
    ++m_pt.idx;
    RunSpec s;
    s.restoreTo = m_pi.target;
    s.noclip = false;
    s.stopAt = static_cast<int64_t>(orig.startFrame) + 93;
    s.clearance = true;
    s.clrFrom = ns;
    s.clrTo = ns + 60;
    s.clrP2 = m_twoPlayer && orig.player2;
    beginRun(pl, Job::ShiftTrial, s, std::move(events));
}

void Engine::nextExtendTrial(PlayLayer* pl) {
    auto const& orig = m_segs[m_pi.seg];
    if (m_pt.idx >= m_pt.cands.size()) {
        finishPolishItem(pl, m_pi.key);
        return;
    }
    int k = m_pt.deltas[m_pt.idx];
    auto events = toEvents(m_pt.cands[m_pt.idx]);
    ++m_pt.idx;
    RunSpec s;
    s.restoreTo = m_pi.target;
    s.noclip = false;
    s.stopAt = static_cast<int64_t>(orig.endFrame) + k + 120;
    s.groundWatch = true;
    s.gFrom = static_cast<int64_t>(orig.endFrame);
    s.gTo = static_cast<int64_t>(orig.endFrame) + k;
    s.gP2 = m_twoPlayer && orig.player2;
    beginRun(pl, Job::ExtendTrial, s, std::move(events));
}

void Engine::finishPolishItem(PlayLayer* pl, std::tuple<int64_t, int, int> key) {
    m_polishKey = key;
    beginBase(pl, m_pi.target);
}

// ---------------------------------------------------------------------------
// run results
// ---------------------------------------------------------------------------

void Engine::finishRun(PlayLayer* pl) {
    m_runActive = false;
    m_res.lastTick = m_lastTick;
    switch (m_job) {
        case Job::Log: onLogDone(pl); break;
        case Job::MergeBase: onMergeBaseDone(pl); break;
        case Job::MergeMerged:
            if (!m_res.died && !m_res.stuck) {
                commitMerge(pl, true);
            } else {
                m_mj.mergedDeath = m_res.died ? m_res.deathTick : -1;
                RunSpec s;
                s.restoreTo = m_mj.E - 1;
                s.noclip = false;
                s.stopAt = m_mj.E + kMergeWindow;
                beginRun(pl, Job::MergeBaseline, s, toEvents(m_segs));
            }
            break;
        case Job::MergeBaseline:
            // Keep the merge if the original clicks die here too, and no later.
            commitMerge(pl, m_res.died && m_mj.mergedDeath >= 0 && m_res.deathTick <= m_mj.mergedDeath);
            break;
        case Job::Base: onBaseDone(pl); break;
        case Job::RepairTrial:
            if (!m_res.died && !m_res.stuck) {
                m_segs = m_fj.current;
                ++m_fixes;
                beginBase(pl, m_fj.target);
            } else {
                tryNextFix(pl);
            }
            break;
        case Job::ShiftTrial:
            m_pt.died.push_back(m_res.died || m_res.stuck);
            m_pt.clear.push_back(m_res.minClear);
            if (m_pt.idx == 1 && (m_res.died || m_res.stuck || m_res.minClear == kNoHazard)) {
                // Nothing dangerous nearby (or the current timing already dies,
                // which the sweep repairs): keep it as it is.
                finishPolishItem(pl, m_pi.key);
            } else {
                nextShiftTrial(pl);
            }
            break;
        case Job::ExtendTrial:
            if (!m_res.died && !m_res.stuck && !m_res.groundTouched) {
                m_segs = m_pt.cands[m_pt.idx - 1];
                ++m_extended;
                finishPolishItem(pl, m_pi.key);
            } else {
                nextExtendTrial(pl);
            }
            break;
        case Job::Verify:
            if (m_res.completed && !m_res.died) {
                succeed(pl);
            } else if (m_res.died && ++m_verifyFails < 3) {
                log::warn("Macro Refiner: verify run died at tick {}, repairing again", m_res.deathTick);
                m_polishDone = true;
                startRepairStage(pl);
            } else {
                fail(pl, m_res.died
                    ? fmt::format("The final check from the start keeps dying (tick {}, {:.0f}%).", m_res.deathTick, percentAt(m_res.deathTick))
                    : std::string("The final check from the start didn't reach the end."));
            }
            break;
        case Job::None: break;
    }
}

void Engine::startVerify(PlayLayer* pl) {
    setStage(Stage::Verify);
    RunSpec s;
    s.restoreTo = -1;
    s.noclip = false;
    beginRun(pl, Job::Verify, s, toEvents(m_segs));
}

// ---------------------------------------------------------------------------
// finishing
// ---------------------------------------------------------------------------

void Engine::succeed(PlayLayer* pl) {
    auto events = toEvents(m_segs);
    auto name = "realistic_output" + outputExtension(m_macro);
    std::string err;
    auto out = m_macro.path.parent_path() / name;
    bool ok = saveMacro(m_macro, events, out, kBotName, err);
    if (!ok) {
        out = Mod::get()->getSaveDir() / name;
        std::string err2;
        ok = saveMacro(m_macro, events, out, kBotName, err2);
        if (!ok) err += "\n" + err2;
    }
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - m_startTime).count();
    m_stage = Stage::Done;
    cleanup(pl, true);
    if (!ok) {
        FLAlertLayer::create("Macro Refiner", fmt::format("Verified at 100%, but saving failed:\n{}", err), "OK")->show();
        return;
    }
    log::info("Macro Refiner: saved {}", out.string());
    FLAlertLayer::create(nullptr, "Macro Refined (100% verified)",
        fmt::format("Saved to:\n<cy>{}</c>\n\nRemoved <cg>{}</c> mid-air taps, merged <cg>{}</c> spam clicks, fixed <cg>{}</c> deaths, "
                    "moved <cg>{}</c> jumps and lengthened <cg>{}</c> holds. {} inputs, {}s.",
                    out.string(), m_removedTaps, m_merges, m_fixes, m_shifted, m_extended, events.size(), secs),
        "OK", nullptr, 400.f)->show();
}

void Engine::fail(PlayLayer* pl, std::string const& why) {
    log::warn("Macro Refiner failed: {}", why);
    m_stage = Stage::Failed;
    cleanup(pl, true);
    FLAlertLayer::create(nullptr, "Macro Refiner", why + "\n\nNothing was saved.", "OK", nullptr, 380.f)->show();
}

void Engine::stop(PlayLayer* pl) {
    if (!running()) return;
    m_stage = Stage::Failed;
    cleanup(pl, true);
    Notification::create("Macro refining stopped", NotificationIcon::Info, 1.5f)->show();
}

void Engine::onQuit(PlayLayer* pl) {
    if (!running()) return;
    m_stage = Stage::Failed;
    cleanup(pl, false);
}

void Engine::cleanup(PlayLayer* pl, bool levelAlive) {
    m_runActive = false;
    m_stepping = false;
    m_job = Job::None;
    m_snaps.clear();
    if (m_label) m_label->removeFromParent();
    if (m_menu) m_menu->removeFromParent();
    m_label = nullptr;
    m_menu = nullptr;
    m_lastLabel.clear();
    m_runEvents.clear();
    m_logP1.clear();
    m_logP1.shrink_to_fit();
    m_logP2.clear();
    m_logP2.shrink_to_fit();
    if (!pl) return;
    CCDirector::get()->getScheduler()->resumeTarget(pl);
    if (!levelAlive) return;
    pl->removeAllCheckpoints();
    if (!m_wasPractice) pl->togglePracticeMode(false);
    pl->resetLevel();
    pl->pauseGame(false);
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

void Engine::createUI(PlayLayer* pl) {
    auto win = CCDirector::get()->getWinSize();
    auto label = CCLabelBMFont::create("Macro Refiner", "chatFont.fnt");
    label->setAnchorPoint({0.5f, 1.f});
    label->setAlignment(kCCTextAlignmentCenter);
    label->setScale(0.6f);
    label->setPosition({win.width / 2.f, win.height - 6.f});
    label->setID("refiner-status"_spr);
    pl->addChild(label, 10000);
    m_label = label;

    auto spr = ButtonSprite::create("Stop", "bigFont.fnt", "GJ_button_06.png", 0.8f);
    spr->setScale(0.55f);
    auto btn = CCMenuItemSpriteExtra::create(spr, StopTarget::shared(), menu_selector(StopTarget::onStop));
    auto menu = CCMenu::create();
    menu->addChild(btn);
    menu->setPosition({win.width - 36.f, 24.f});
    menu->setID("refiner-stop"_spr);
    if (pl->m_uiLayer) pl->m_uiLayer->addChild(menu, 1000);
    else pl->addChild(menu, 10000);
    m_menu = menu;
}

std::string Engine::statusText() const {
    static char const* names[] = {"", "Reading macro (noclip)", "Step 1: merging spam clicks", "Step 2: fixing deaths",
                                  "Step 3: humanizing", "Final check from 0%", "", ""};
    float stageLo = 0.f, stageW = 10.f;
    switch (m_stage) {
        case Stage::Log: stageLo = 0.f; stageW = 10.f; break;
        case Stage::Merge: stageLo = 10.f; stageW = 20.f; break;
        case Stage::Repair: stageLo = 30.f; stageW = 30.f; break;
        case Stage::Polish: stageLo = 60.f; stageW = 30.f; break;
        case Stage::Verify: stageLo = 90.f; stageW = 10.f; break;
        default: break;
    }
    float frac = 0.f;
    if (m_stage == Stage::Log) {
        int64_t est = m_macro.events.empty() ? 1 : std::max<int64_t>(1, m_macro.events.back().frame);
        frac = std::clamp(static_cast<float>(m_progressTick) / static_cast<float>(est), 0.f, 1.f);
    } else if (m_endTick > 0) {
        frac = std::clamp(static_cast<float>(m_progressTick) / static_cast<float>(m_endTick), 0.f, 1.f);
    }
    float total = stageLo + stageW * frac;
    auto text = fmt::format("Macro Refiner {:.0f}%  -  {} ({:.0f}%)\nremoved {}  merged {}  fixed {}  moved {}  longer {}  |  sims {}",
                            total, names[static_cast<int>(m_stage)], frac * 100.f, m_removedTaps, m_merges, m_fixes,
                            m_shifted, m_extended, m_trials);
    if (m_restoreDrift > 0) text += fmt::format("  drift {}", m_restoreDrift);
    return text;
}

void Engine::updateUI() {
    if (!m_label) return;
    auto text = statusText();
    if (text != m_lastLabel) {
        m_lastLabel = text;
        m_label->setString(text.c_str());
    }
}

} // namespace mr
