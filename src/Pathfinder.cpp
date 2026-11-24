#include "Pathfinder.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>

using namespace geode::prelude;

namespace pf {

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

namespace {

constexpr uint64_t FNV_OFFSET = 1469598103934665603ull;
constexpr uint64_t FNV_PRIME = 1099511628211ull;

void fnv(uint64_t& h, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        h ^= (v >> (i * 8)) & 0xFF;
        h *= FNV_PRIME;
    }
}

bool settingBool(char const* key) { return Mod::get()->getSettingValue<bool>(key); }

std::optional<ObjKind> classify(GameObjectType t) {
    switch (t) {
        case GameObjectType::Solid:
        case GameObjectType::Slope:
        case GameObjectType::Breakable:
            return ObjKind::Solid;
        case GameObjectType::Hazard:
        case GameObjectType::AnimatedHazard:
            return ObjKind::Hazard;
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
            return ObjKind::Orb;
        case GameObjectType::YellowJumpPad:
        case GameObjectType::PinkJumpPad:
        case GameObjectType::GravityPad:
        case GameObjectType::RedJumpPad:
        case GameObjectType::SpiderPad:
            return ObjKind::Pad;
        case GameObjectType::InverseGravityPortal:
        case GameObjectType::NormalGravityPortal:
        case GameObjectType::ShipPortal:
        case GameObjectType::CubePortal:
        case GameObjectType::InverseMirrorPortal:
        case GameObjectType::NormalMirrorPortal:
        case GameObjectType::BallPortal:
        case GameObjectType::RegularSizePortal:
        case GameObjectType::MiniSizePortal:
        case GameObjectType::UfoPortal:
        case GameObjectType::DualPortal:
        case GameObjectType::SoloPortal:
        case GameObjectType::WavePortal:
        case GameObjectType::RobotPortal:
        case GameObjectType::TeleportPortal:
        case GameObjectType::SpiderPortal:
        case GameObjectType::SwingPortal:
        case GameObjectType::GravityTogglePortal:
            return ObjKind::Portal;
        default:
            return std::nullopt;
    }
}

// Speed portals by object ID -> units per second (0.5x, 1x, 2x, 3x, 4x).
float speedPortalSpeed(int id) {
    switch (id) {
        case 200: return 251.16f;
        case 201: return 311.58f;
        case 202: return 387.42f;
        case 203: return 468.0f;
        case 1334: return 576.0f;
        default: return 0.f;
    }
}

std::optional<ObjKind> classifyObj(GameObject* o) {
    if (speedPortalSpeed(o->m_objectID) > 0.f) return ObjKind::Speed;
    return classify(o->m_objectType);
}

bool stateFrom(std::vector<Event> const& events, uint8_t ch, uint64_t tick, bool startHold) {
    bool h = startHold;
    for (auto const& e : events) {
        if (e.ch != ch) continue;
        if (e.tick > tick) break;
        h = e.hold;
    }
    return h;
}

void sortEvents(std::vector<Event>& v) {
    std::stable_sort(v.begin(), v.end(), [](Event const& a, Event const& b) { return a.tick < b.tick; });
}

int bucketOf(float x) { return static_cast<int>(std::floor(x / 100.f)); }

int64_t absDiff(int64_t a, int64_t b) { return a > b ? a - b : b - a; }

constexpr uint64_t SKIP_TOKEN_TICKS = 600; // a game respawn timer we beat must fire within this many ticks

} // namespace

char const* modeName(Mode m) {
    switch (m) {
        case Mode::Cube: return "Cube";
        case Mode::Ship: return "Ship";
        case Mode::Ball: return "Ball";
        case Mode::Ufo: return "UFO";
        case Mode::Wave: return "Wave";
        case Mode::Robot: return "Robot";
        case Mode::Spider: return "Spider";
        case Mode::Swing: return "Swing";
    }
    return "?";
}

bool isFlyMode(Mode m) {
    return m == Mode::Ship || m == Mode::Ufo || m == Mode::Wave || m == Mode::Swing;
}

bool isBoundedMode(Mode m) { return isFlyMode(m) || m == Mode::Ball || m == Mode::Spider; }

TraceTick const* Trace::at(uint8_t ch, uint64_t tick) const {
    if (ch > 1 || tick < base) return nullptr;
    auto idx = static_cast<size_t>(tick - base);
    if (idx >= ticks[ch].size()) return nullptr;
    return &ticks[ch][idx];
}

TraceTick const* Trace::last(uint8_t ch) const {
    if (ch > 1 || ticks[ch].empty()) return nullptr;
    return &ticks[ch].back();
}

Pathfinder& Pathfinder::get() {
    static Pathfinder inst;
    return inst;
}

// ---------------------------------------------------------------------------
// profile / settings
// ---------------------------------------------------------------------------

int Pathfinder::maxFailures() const {
    int fixed = profileSpec(m_profile).maxFailures;
    if (fixed > 0) return fixed; // Mode 2: 65, Mode 3: 120
    return static_cast<int>(std::clamp<int64_t>(Mod::get()->getSettingValue<int64_t>("max-failures"), 10, 200));
}

uint64_t Pathfinder::nextCpInterval(uint64_t seed) const {
    auto const& sp = profileSpec(m_profile);
    auto span = static_cast<float>(sp.cpMax - sp.cpMin + 1);
    return sp.cpMin + std::min<uint64_t>(sp.cpMax - sp.cpMin, static_cast<uint64_t>(Humanizer::hash01(seed) * span));
}

void Pathfinder::setProfile(Profile p, PlayLayer* pl) {
    (void)pl;
    m_profile = p;
    Mod::get()->setSavedValue<int64_t>("profile", static_cast<int64_t>(p));
    m_status = fmt::format("Mode {}", profileLabel(p));
    if (m_segments.empty()) return;
    auto& s = seg();
    s.human = Humanizer::makeParams(s.startTick * 31 + m_segments.size() * 7919 + 17 + static_cast<uint64_t>(p), p);
    m_human.setParams(s.human);
    m_human.clearHistory();
    m_cpInterval = nextCpInterval(m_lastCheckpointTick * 13 + static_cast<uint64_t>(p));
    m_pendingToggleAt[0] = m_pendingToggleAt[1] = -1;
    s.pendingPolish.reset();
    if (s.cluster) {
        // Re-plan the spot being fixed with the new profile's rules.
        auto& c = *s.cluster;
        c.pairsAdded = c.microAdded = c.sweepAdded = false;
        buildQueue(s, c);
    }
}

// ---------------------------------------------------------------------------
// state / lifecycle
// ---------------------------------------------------------------------------

bool Pathfinder::active(PlayLayer* pl) const {
    if (!m_enabled || !m_inLevel || !pl) return false;
    if (!pl->m_isPracticeMode) return false;
    if (pl->m_isPlatformer) return false;
    if (pl->m_levelEndAnimationStarted) return false;
    return true;
}

bool Pathfinder::shouldBlockManualInput(PlayLayer* pl) const {
    return !m_sending && active(pl) && settingBool("block-manual-input");
}

void Pathfinder::setEnabled(bool on, PlayLayer* pl) {
    if (m_enabled == on) return;
    m_enabled = on;
    m_respawnPending = false;
    m_skipTokens.clear();
    m_fastForward = false;
    if (!pl) return;
    if (!on) {
        releaseAll(pl);
        m_pendingCheckpoint = false;
        m_status = "Paused (F6 to resume)";
    } else {
        m_lastTick = UINT64_MAX;
        m_havePrevY[0] = m_havePrevY[1] = false;
        if (m_index.empty()) buildIndex(pl);
        syncSegments(pl);
        // Turning on mid-attempt: everything from here on is live play.
        auto now = static_cast<uint64_t>(pl->m_gameState.m_currentProgress);
        auto& s = seg();
        s.plan.erase(std::remove_if(s.plan.begin(), s.plan.end(), [&](Event const& e) { return e.tick >= now; }), s.plan.end());
        s.lockedUntil = std::min(s.lockedUntil, now);
        s.activePolish.reset();
        m_trace = {};
        m_trace.base = now;
        m_lastCheckpointTick = now;
        m_status = pl->m_isPracticeMode ? "Solving" : "Enter Practice Mode to start";
    }
}

void Pathfinder::onLevelInit(PlayLayer* pl) {
    m_inLevel = true;
    m_enabled = settingBool("enabled");
    {
        auto saved = Mod::get()->getSavedValue<int64_t>("profile", 1);
        m_profile = static_cast<Profile>(std::clamp<int64_t>(saved, 1, 3));
    }
    m_segments.clear();
    m_doomed.clear();
    m_index.clear();
    m_wide.clear();
    m_live.clear();
    m_liveSet.clear();
    m_liveTick = UINT64_MAX;
    m_trace = {};
    m_totalDeaths = 0;
    m_hold[0] = m_hold[1] = false;
    m_lastTick = UINT64_MAX;
    m_respawnPending = false;
    m_skipTokens.clear();
    m_fastRespawnBroken = false;
    m_fastForward = false;
    for (int c = 0; c < 2; c++) {
        m_learnedFloor[c].clear();
        m_learnedCeil[c].clear();
        m_modeKnown[c] = false;
    }
    m_brainPlan[0] = m_brainPlan[1] = {};
    if (!m_modelLoaded) {
        m_modelLoaded = true;
        m_model.deserialize(Mod::get()->getSavedValue<std::string>("brain-model", ""));
    }
    m_status = "Enter Practice Mode to start";
    (void)pl;
}

void Pathfinder::saveModel() {
    if (m_model.samples() > 0) Mod::get()->setSavedValue<std::string>("brain-model", m_model.serialize());
}

bool Pathfinder::brainOn() const { return settingBool("brain"); }

void Pathfinder::onLevelExit() {
    saveModel();
    m_inLevel = false;
    m_segments.clear();
    m_doomed.clear();
    m_index.clear();
    m_wide.clear();
    m_live.clear();
    m_liveSet.clear();
    m_trace = {};
    m_hold[0] = m_hold[1] = false;
    m_pendingCheckpoint = false;
    m_respawnPending = false;
    m_skipTokens.clear();
    m_fastForward = false;
}

int Pathfinder::channels(PlayLayer* pl) const {
    if (pl->m_levelSettings && pl->m_levelSettings->m_twoPlayerMode && pl->m_gameState.m_isDualMode) return 2;
    return 1;
}

Segment Pathfinder::makeSegment(uint64_t startTick, bool hold0, bool hold1, bool ours) {
    Segment s;
    s.startTick = startTick;
    s.startHold[0] = hold0;
    s.startHold[1] = hold1;
    s.ours = ours;
    s.human = Humanizer::makeParams(startTick * 31 + m_segments.size() * 7919 + 17, m_profile);
    if (hold0) s.plan.push_back({startTick, true, 0});
    if (hold1) s.plan.push_back({startTick, true, 1});
    s.lockedUntil = (hold0 || hold1) ? startTick + 1 : startTick;
    return s;
}

void Pathfinder::syncSegments(PlayLayer* pl) {
    size_t cps = pl->m_checkpointArray ? pl->m_checkpointArray->count() : 0;
    auto now = static_cast<uint64_t>(pl->m_gameState.m_currentProgress);
    if (m_segments.empty()) m_segments.push_back(makeSegment(0, false, false, true));
    while (m_segments.size() > cps + 1) m_segments.pop_back(); // checkpoints deleted by hand
    while (m_segments.size() < cps + 1) m_segments.push_back(makeSegment(now, false, false, false)); // placed by hand
}

void Pathfinder::onCheckpointAdded(PlayLayer* pl, uint64_t tick, bool ours) {
    if (m_segments.empty()) syncSegments(pl);
    auto& parent = seg();
    parent.lastTrace = m_trace;
    // A polish still being tested in the parent stays as it is: the run got
    // past it, so it works.
    parent.activePolish.reset();
    Segment child = makeSegment(tick, m_hold[0], m_hold[1], ours);
    if (auto lt = m_trace.last(0)) {
        child.startState = *lt;
        child.hasStartState = true;
    }
    m_segments.push_back(std::move(child));
    m_trace = {};
    m_trace.base = tick;
    m_lastCheckpointTick = tick;
    m_lastSpecialPass = 0;
    m_cpInterval = nextCpInterval(tick * 13 + 5);
    m_human.setParams(seg().human);
    m_human.clearHistory();
    m_fastForward = false;
    m_status = "Checkpoint placed";
}

void Pathfinder::onBeforeReset(PlayLayer* pl) {
    if (!m_inLevel) return;
    m_wasAliveBeforeReset = pl->m_player1 && !pl->m_player1->m_isDead;
    releaseAll(pl);
}

void Pathfinder::onReset(PlayLayer* pl) {
    if (!m_inLevel) return;

    // Our instant respawn beat the game's respawn timer. If the game then
    // resets a live player some other way than through delayedResetLevel,
    // instant respawn isn't compatible with this setup: switch it off.
    if (!m_inOwnReset && m_wasAliveBeforeReset && active(pl)) {
        auto stillPending = std::any_of(m_skipTokens.begin(), m_skipTokens.end(), [&](uint64_t t) {
            return m_tickCounter - t < SKIP_TOKEN_TICKS;
        });
        if (stillPending && !m_fastRespawnBroken) {
            m_fastRespawnBroken = true;
            log::warn("Pathfinder: game reset a live player after an instant respawn; turning instant respawn off");
        }
    }
    m_respawnPending = false;
    m_fastForward = false;

    m_hold[0] = m_hold[1] = false;
    m_lastTick = UINT64_MAX;
    for (int c = 0; c < 2; c++) {
        m_pendingToggleAt[c] = -1;
        m_tapReleaseAt[c] = -1;
        m_havePrevY[c] = false;
        m_modeKnown[c] = false;
        m_lastToggle[c] = 0;
        m_lastPress[c] = -100000;
        m_assistHold[c] = 0;
        m_vySmooth[c] = 0.f;
        m_brainPlan[c] = {};
        m_lastGuardTick[c] = 0;
    }
    m_deathHandled = false;
    m_divergedThisAttempt = false;
    m_pendingCheckpoint = false;
    m_assistJumpedFor = 0;
    m_lastSpecialPass = 0;
    m_liveTick = UINT64_MAX;

    if (m_index.empty()) buildIndex(pl); // once per level: positions are read live
    syncSegments(pl);

    auto now = static_cast<uint64_t>(pl->m_gameState.m_currentProgress);
    auto& s = seg();
    s.attempts++;
    if (s.attempts == 1 && s.startTick != now && s.plan.empty()) s.startTick = now;

    // Throw away what the live policy played last attempt; the locked part stays.
    s.plan.erase(std::remove_if(s.plan.begin(), s.plan.end(), [&](Event const& e) { return e.tick >= s.lockedUntil; }), s.plan.end());

    m_trace = {};
    m_trace.base = now;
    m_lastCheckpointTick = now;
    m_cpInterval = nextCpInterval(now * 13 + s.attempts);
    if (s.human.profile != m_profile) {
        s.human = Humanizer::makeParams(s.startTick * 31 + m_segments.size() * 7919 + 17 + static_cast<uint64_t>(m_profile), m_profile);
    }
    m_human.setParams(s.human);
    m_human.reseed(now * 1000003ull + static_cast<uint64_t>(s.attempts) * 7 + m_segments.size());
    m_human.clearHistory();
    m_prevXs[0] = m_prevXs[1] = 0;
    m_prevX = 0;

    if (!active(pl)) {
        m_status = m_enabled ? "Enter Practice Mode to start" : "Off (F6 to start)";
    } else if (!s.cluster) {
        m_status = "Exploring";
    }
}

// ---------------------------------------------------------------------------
// instant respawn + fast-forward
// ---------------------------------------------------------------------------

void Pathfinder::onFrame(PlayLayer* pl) {
    m_frame++;
    if (!m_respawnPending) return;
    if (!pl || !active(pl) || !settingBool("instant-respawn") || m_fastRespawnBroken) {
        m_respawnPending = false;
        return;
    }
    if (!pl->m_player1 || !pl->m_player1->m_isDead) {
        m_respawnPending = false; // already respawned
        return;
    }
    // Two frames: lets the game finish the death bookkeeping first.
    if (++m_respawnFrames < 2) return;
    m_respawnPending = false;
    m_skipTokens.push_back(m_tickCounter);
    m_lastFastResetFrame = m_frame;
    m_inOwnReset = true;
    pl->delayedResetLevel(); // the same call the game's own respawn timer makes
    m_inOwnReset = false;
}

bool Pathfinder::shouldSkipDelayedReset(PlayLayer* pl) {
    if (m_inOwnReset) return false;
    m_skipTokens.erase(std::remove_if(m_skipTokens.begin(), m_skipTokens.end(),
                                      [&](uint64_t t) { return m_tickCounter - t >= SKIP_TOKEN_TICKS; }),
                       m_skipTokens.end());
    if (m_skipTokens.empty()) return false;
    if (!pl || !pl->m_player1 || pl->m_player1->m_isDead) return false; // a real pending respawn
    m_skipTokens.erase(m_skipTokens.begin());
    return true;
}

float Pathfinder::timeMultiplier(PlayLayer* pl) const {
    if (!m_fastForward || !pl || !active(pl) || !settingBool("fast-forward")) return 1.f;
    if (!pl->m_player1 || pl->m_player1->m_isDead) return 1.f;
    auto v = Mod::get()->getSettingValue<double>("fast-forward-speed");
    return static_cast<float>(std::clamp(v, 1.0, 50.0));
}

// ---------------------------------------------------------------------------
// world inspection
// ---------------------------------------------------------------------------

void Pathfinder::buildIndex(PlayLayer* pl) {
    m_index.clear();
    m_wide.clear();
    m_maxWidth = 0;
    if (!pl->m_objects) return;
    m_index.reserve(pl->m_objects->count() / 2);
    for (auto obj : CCArrayExt<GameObject*>(pl->m_objects)) {
        if (!obj) continue;
        auto kind = classifyObj(obj);
        if (!kind) continue;
        if ((*kind == ObjKind::Solid || *kind == ObjKind::Hazard) && obj->m_isNoTouch) continue;
        auto const& r = obj->getObjectRect();
        if (r.size.width <= 0.f && r.size.height <= 0.f) continue;
        WorldObject w{r.getMinX(), r.getMaxX(), obj, *kind};
        if (r.size.width > 600.f) {
            m_wide.push_back(w);
        } else {
            m_maxWidth = std::max(m_maxWidth, r.size.width);
            m_index.push_back(w);
        }
    }
    std::sort(m_index.begin(), m_index.end(), [](WorldObject const& a, WorldObject const& b) { return a.minX < b.minX; });
}

void Pathfinder::refreshLive(PlayLayer* pl, uint64_t tick) {
    // The game keeps every object in a grid of sections and moves objects
    // between sections as move triggers push them around; its collision
    // code reads the active sections every tick. Reading the same lists
    // gives the real, current positions of moving blocks and hazards.
    if (tick == m_liveTick) return;
    m_liveTick = tick;
    m_live.clear();
    m_liveSet.clear();
    auto& secs = pl->m_sections;
    auto& sizes = pl->m_sectionSizes;
    int l = std::max(0, pl->m_leftSectionIndex);
    int r = pl->m_rightSectionIndex;
    int b = std::max(0, pl->m_bottomSectionIndex);
    int t = pl->m_topSectionIndex;
    if (r < l || t < b || r - l > 400 || t - b > 400) return;
    for (int i = l; i <= r && i < static_cast<int>(secs.size()) && i < static_cast<int>(sizes.size()); i++) {
        auto col = secs[i];
        auto colSizes = sizes[i];
        if (!col || !colSizes) continue;
        for (int j = b; j <= t && j < static_cast<int>(col->size()) && j < static_cast<int>(colSizes->size()); j++) {
            auto sec = (*col)[j];
            if (!sec) continue;
            int n = std::min((*colSizes)[j], static_cast<int>(sec->size()));
            for (int k = 0; k < n; k++) {
                GameObject* obj = (*sec)[k];
                if (!obj) continue;
                auto kind = classifyObj(obj);
                if (!kind) continue;
                if ((*kind == ObjKind::Solid || *kind == ObjKind::Hazard) && obj->m_isNoTouch) continue;
                if (!m_liveSet.insert(obj).second) continue;
                auto const& rc = obj->getObjectRect();
                m_live.push_back({rc.getMinX(), rc.getMaxX(), obj, *kind});
            }
        }
    }
}

void Pathfinder::queryWindow(float x0, float x1, std::vector<WorldObject const*>& out) const {
    out.clear();
    for (auto const& w : m_live) {
        if (w.minX <= x1 && w.maxX >= x0) out.push_back(&w);
    }
    // Beyond what the game currently simulates: the level's layout.
    float from = x0 - m_maxWidth - 1.f;
    auto it = std::lower_bound(m_index.begin(), m_index.end(), from,
                               [](WorldObject const& w, float v) { return w.minX < v; });
    for (; it != m_index.end() && it->minX <= x1; ++it) {
        if (it->maxX >= x0 && !m_liveSet.count(it->obj)) out.push_back(&*it);
    }
    for (auto const& w : m_wide) {
        if (w.minX <= x1 && w.maxX >= x0 && !m_liveSet.count(w.obj)) out.push_back(&w);
    }
}

PlayerSnap Pathfinder::snap(PlayerObject* p, uint8_t ch) const {
    PlayerSnap s;
    if (!p) return s;
    s.valid = true;
    s.x = p->m_position.x;
    s.y = p->m_position.y;
    s.yVel = p->m_yVelocity;
    if (p->m_isShip) s.mode = Mode::Ship;
    else if (p->m_isBird) s.mode = Mode::Ufo;
    else if (p->m_isBall) s.mode = Mode::Ball;
    else if (p->m_isDart) s.mode = Mode::Wave;
    else if (p->m_isRobot) s.mode = Mode::Robot;
    else if (p->m_isSpider) s.mode = Mode::Spider;
    else if (p->m_isSwing) s.mode = Mode::Swing;
    else s.mode = Mode::Cube;
    s.upsideDown = p->m_isUpsideDown;
    s.onGround = p->m_isOnGround;
    s.dead = p->m_isDead;
    s.size = p->m_vehicleSize;
    s.mini = p->m_vehicleSize < 0.9f;
    s.rect = p->getObjectRect();
    if (s.rect.size.height <= 0.f) {
        float h = 30.f * s.size;
        s.rect = CCRect(s.x - h / 2, s.y - h / 2, h, h);
    }
    if (ch <= 1 && m_havePrevY[ch]) {
        s.vy = s.y - m_prevY[ch];
        s.vx = s.x - m_prevXs[ch];
    }
    return s;
}

bool Pathfinder::touchingOrb(PlayerSnap const& s) const {
    std::vector<WorldObject const*> objs;
    queryWindow(s.rect.getMinX() - 5.f, s.rect.getMaxX() + 5.f, objs);
    for (auto w : objs) {
        if (w->kind != ObjKind::Orb) continue;
        if (w->obj->getObjectRect().intersectsRect(s.rect)) return true;
    }
    return false;
}

int Pathfinder::touchingType(PlayerSnap const& s, ObjKind kind) const {
    std::vector<WorldObject const*> objs;
    queryWindow(s.rect.getMinX() - 5.f, s.rect.getMaxX() + 5.f, objs);
    for (auto w : objs) {
        if (w->kind != kind) continue;
        if (w->obj->getObjectRect().intersectsRect(s.rect)) return static_cast<int>(w->obj->m_objectType);
    }
    return -1;
}

bool Pathfinder::solidContact(PlayerSnap const& s) const {
    std::vector<WorldObject const*> objs;
    CCRect grown(s.rect.getMinX() - 3.f, s.rect.getMinY() - 3.f, s.rect.size.width + 6.f, s.rect.size.height + 6.f);
    queryWindow(grown.getMinX(), grown.getMaxX(), objs);
    for (auto w : objs) {
        if (w->kind != ObjKind::Solid) continue;
        if (w->obj->getObjectRect().intersectsRect(grown)) return true;
    }
    return false;
}

void Pathfinder::updateBounds(uint8_t ch, PlayerSnap const& s) {
    if (ch > 1) return;
    // Entering a flying mode (or respawning in one): guess the bounds the
    // way the game frames them - 10 blocks, centred on the snapped entry
    // height, never below the real ground.
    if (!m_modeKnown[ch] || m_prevMode[ch] != s.mode) {
        m_modeKnown[ch] = true;
        m_prevMode[ch] = s.mode;
        if (isBoundedMode(s.mode)) {
            float height = isFlyMode(s.mode) ? 300.f : 240.f; // ball/spider sections are 8 blocks
            float snapped = std::round(s.y / 30.f) * 30.f;
            m_modeFloor[ch] = std::max(90.f, snapped - height * 0.5f);
            m_modeCeil[ch] = m_modeFloor[ch] + height;
        }
    }
    if (!isBoundedMode(s.mode)) return;
    // Sliding on an invisible bound with no block involved: that is the
    // real floor/ceiling. Learn it for this part of the level.
    if (s.onGround && !solidContact(s)) {
        int b = bucketOf(s.x);
        bool nearFloor = (s.y - m_modeFloor[ch]) < (m_modeCeil[ch] - s.y);
        if (nearFloor) {
            float f = s.rect.getMinY();
            m_learnedFloor[ch][b] = f;
            m_modeFloor[ch] = f;
            if (m_modeCeil[ch] < f + 60.f) m_modeCeil[ch] = f + 300.f;
        } else {
            float c = s.rect.getMaxY();
            m_learnedCeil[ch][b] = c;
            m_modeCeil[ch] = c;
            if (m_modeFloor[ch] > c - 60.f) m_modeFloor[ch] = std::max(90.f, c - 300.f);
        }
    }
}

Corridor Pathfinder::corridor(PlayLayer* pl, uint8_t ch, PlayerSnap const& s, float lookahead) const {
    (void)pl;
    Corridor c;
    if (ch > 1) return c;
    float half = s.rect.size.height * 0.5f;
    float floorY = m_modeFloor[ch];
    float ceilY = m_modeCeil[ch];
    int b = bucketOf(s.x);
    for (int d = -1; d <= 2; d++) {
        auto f = m_learnedFloor[ch].find(b + d);
        if (f != m_learnedFloor[ch].end()) floorY = std::max(floorY, f->second);
        auto k = m_learnedCeil[ch].find(b + d);
        if (k != m_learnedCeil[ch].end()) ceilY = std::min(ceilY, k->second);
    }
    // Defaults that clearly don't contain the player are stale guesses.
    if (s.y - half < floorY - 2.f) floorY = std::max(90.f, s.y - half - 150.f);
    if (s.y + half > ceilY + 2.f) ceilY = s.y + half + 150.f;

    std::vector<WorldObject const*> objs;
    queryWindow(s.x - 10.f, s.x + lookahead, objs);
    for (auto w : objs) {
        if (w->kind != ObjKind::Solid && w->kind != ObjKind::Hazard) continue;
        auto const& r = w->obj->getObjectRect();
        if (r.getMaxY() < floorY || r.getMinY() > ceilY) continue;
        if (r.getMidY() <= s.y) floorY = std::max(floorY, r.getMaxY());
        else ceilY = std::min(ceilY, r.getMinY());
    }
    float margin = (s.mode == Mode::Wave) ? 4.f : 6.f;
    c.floor = floorY;
    c.ceil = ceilY;
    c.yMin = floorY + half + margin;
    c.yMax = ceilY - half - margin;
    if (c.yMax < c.yMin) {
        float mid = (c.yMin + c.yMax) * 0.5f;
        c.yMin = c.yMax = mid;
    }
    c.valid = true;
    return c;
}

// ---------------------------------------------------------------------------
// input
// ---------------------------------------------------------------------------

void Pathfinder::press(PlayLayer* pl, uint8_t ch, bool down, uint64_t tick, bool record) {
    if (ch > 1 || m_hold[ch] == down) return;
    bool isPlayer1 = ch == 0;
    if (pl->m_levelSettings && pl->m_levelSettings->m_twoPlayerMode && GameManager::get()->getGameVariable("0010")) {
        isPlayer1 = !isPlayer1; // "flip 2-player controls"
    }
    m_sending = true;
    pl->handleButton(down, 1, isPlayer1);
    m_sending = false;
    m_hold[ch] = down;
    m_lastToggle[ch] = tick;
    if (down) m_lastPress[ch] = static_cast<int64_t>(tick);
    if (ch == 0) m_human.noteToggle(tick);
    if (record && !m_segments.empty()) seg().plan.push_back({tick, down, ch});
}

void Pathfinder::releaseAll(PlayLayer* pl) {
    for (uint8_t ch = 0; ch < 2; ch++) {
        m_pendingToggleAt[ch] = -1;
        m_tapReleaseAt[ch] = -1;
        if (m_hold[ch]) press(pl, ch, false, m_lastTick == UINT64_MAX ? 0 : m_lastTick, false);
    }
}

bool Pathfinder::planStateAt(Segment const& s, uint8_t ch, uint64_t tick) const {
    return stateFrom(s.plan, ch, tick, false);
}

int Pathfinder::spotFailures(uint64_t tick) const {
    if (m_segments.empty()) return 0;
    auto const& s = m_segments.back();
    if (!s.cluster) return 0;
    auto const& c = *s.cluster;
    if (tick + 30 >= c.windowStart && tick <= c.deathTick + 60) return c.tries;
    return 0;
}

FlyParams Pathfinder::flyParamsAt(uint8_t ch, uint64_t tick) const {
    if (m_segments.empty()) return {};
    auto const& s = m_segments.back();
    if (s.flyOverride && s.flyOverride->ch == ch && tick >= s.flyOverride->from && tick <= s.flyOverride->to) {
        return s.flyOverride->params;
    }
    return {};
}

std::optional<bool> Pathfinder::livePolicy(PlayLayer* pl, uint8_t ch, PlayerSnap const& s, uint64_t tick) {
    if (m_tapReleaseAt[ch] >= 0 && static_cast<int64_t>(tick) >= m_tapReleaseAt[ch]) {
        m_tapReleaseAt[ch] = -1;
        return false;
    }
    if (m_pendingToggleAt[ch] >= 0) {
        if (static_cast<int64_t>(tick) >= m_pendingToggleAt[ch]) {
            m_pendingToggleAt[ch] = -1;
            return m_pendingToggleTo[ch];
        }
        return std::nullopt;
    }
    if (m_tapReleaseAt[ch] >= 0) return std::nullopt; // mid-tap
    if (isFlyMode(s.mode)) return flyGuard(pl, ch, s, tick, flyController(pl, ch, s, tick));
    if (brainOn()) return brainGround(pl, ch, s, tick);
    return groundAssist(s, ch, tick);
}

std::optional<bool> Pathfinder::flyController(PlayLayer* pl, uint8_t ch, PlayerSnap const& s, uint64_t tick) {
    FlyParams fp = flyParamsAt(ch, tick);
    float vx = std::max(s.vx, 1.f);
    float look = std::clamp(vx * 45.f * fp.leadScale, 60.f, 260.f);
    Corridor c = corridor(pl, ch, s, look);
    if (!c.valid) return std::nullopt;

    float height = c.yMax - c.yMin;
    float center = (c.yMin + c.yMax) * 0.5f + fp.centerOffset + m_human.drift(tick, height);
    if (height > 4.f) center = std::clamp(center, c.yMin + 2.f, c.yMax - 2.f);
    else center = (c.yMin + c.yMax) * 0.5f;
    float band = m_human.band(height, fp.bandScale);

    int leadTicks = 10;
    switch (s.mode) {
        case Mode::Wave: leadTicks = 3; break;
        case Mode::Ufo: leadTicks = 12; break;
        case Mode::Swing: leadTicks = 9; break;
        default: break;
    }
    leadTicks = std::max(1, static_cast<int>(std::lround(static_cast<float>(leadTicks) * fp.leadScale)));
    float py = s.y + m_vySmooth[ch] * static_cast<float>(leadTicks);
    float lowEdge = center - band;
    float highEdge = center + band;
    bool up = !s.upsideDown; // pressing pushes towards +y in normal gravity
    bool hold = m_hold[ch];
    int fails = spotFailures(tick);
    int floorT = m_human.minStateTicks(fails);
    bool isTap = (s.mode == Mode::Ufo || s.mode == Mode::Swing);

    std::optional<bool> want;
    if (s.mode == Mode::Ship || s.mode == Mode::Wave) {
        if (!hold) {
            if (up ? py < lowEdge : py > highEdge) want = true;
        } else {
            if (up ? py > highEdge : py < lowEdge) want = false;
        }
    } else if (s.mode == Mode::Ufo) {
        if (hold) {
            if (tick - m_lastToggle[ch] >= static_cast<uint64_t>(floorT)) want = false;
        } else if (up ? py < lowEdge : py > highEdge) {
            want = true;
        }
    } else if (s.mode == Mode::Swing) {
        if (hold) {
            if (tick - m_lastToggle[ch] >= static_cast<uint64_t>(floorT)) want = false;
        } else {
            bool headingDown = s.vy < 0.f;
            if ((py < lowEdge && headingDown) || (py > highEdge && !headingDown)) want = true;
        }
    }
    if (!want) return std::nullopt;

    // 15 CPS hard cap (Modes 2/3): >= 16 ticks between two presses.
    int cooldown = m_human.pressCooldown();
    if (*want && cooldown > 0 && static_cast<int64_t>(tick) - m_lastPress[ch] < cooldown) return std::nullopt;

    uint64_t since = tick - m_lastToggle[ch];
    if (since < static_cast<uint64_t>(floorT)) {
        // Hold/release floor. Only an about-to-crash prediction on a spot
        // that already failed a lot may go down to a micro toggle.
        bool emergency = py < c.yMin - 2.f || py > c.yMax + 2.f;
        if (!(emergency && m_human.microAllowed(fails) && since >= static_cast<uint64_t>(m_human.microTicks()))) {
            return std::nullopt;
        }
    }

    bool safe = py > c.yMin + band * 0.5f && py < c.yMax - band * 0.5f;
    int jitter = m_human.toggleJitter(safe);
    if (*want && isTap) {
        int tapLen = std::max(floorT, Humanizer::tapLength(m_human.params(), tick * 7 + ch));
        m_tapReleaseAt[ch] = static_cast<int64_t>(tick) + jitter + tapLen;
    }
    if (jitter > 0) {
        m_pendingToggleAt[ch] = static_cast<int64_t>(tick) + jitter;
        m_pendingToggleTo[ch] = *want;
        return std::nullopt;
    }
    return want;
}

std::optional<bool> Pathfinder::groundAssist(PlayerSnap const& s, uint8_t ch, uint64_t tick) {
    int floorT = m_human.minStateTicks(spotFailures(tick));
    if (m_hold[ch]) {
        // Default for ground modes is "nothing held": let go after a natural tap.
        int len = std::max(floorT, m_assistHold[ch]);
        if (tick - m_lastToggle[ch] >= static_cast<uint64_t>(len)) {
            m_assistHold[ch] = 0;
            return false;
        }
        return std::nullopt;
    }
    if (!settingBool("reactive-assist")) return std::nullopt;
    if (s.mode != Mode::Cube && s.mode != Mode::Robot) return std::nullopt;
    if (!s.onGround || s.vx <= 0.5f) return std::nullopt;
    if (tick - m_lastToggle[ch] < static_cast<uint64_t>(floorT)) return std::nullopt;
    int cooldown = m_human.pressCooldown();
    if (cooldown > 0 && static_cast<int64_t>(tick) - m_lastPress[ch] < cooldown) return std::nullopt;

    float air = s.mode == Mode::Robot ? 70.f : (s.mini ? 72.f : 88.f);
    float span = s.vx * air;
    std::vector<WorldObject const*> objs;
    queryWindow(s.rect.getMaxX(), s.rect.getMaxX() + span * 0.4f, objs);
    float half = s.rect.size.height * 0.5f;
    float best = 1e9f;
    GameObject* bestObj = nullptr;
    for (auto w : objs) {
        if (w->kind != ObjKind::Hazard) continue;
        auto const& r = w->obj->getObjectRect();
        if (r.getMinY() > s.y + half + 6.f || r.getMaxY() < s.y - half - 6.f) continue; // not in our lane
        float dist = r.getMinX() - s.rect.getMaxX();
        if (dist < 0.f || dist >= best) continue;
        best = dist;
        bestObj = w->obj;
    }
    if (!bestObj) return std::nullopt;
    auto key = reinterpret_cast<uintptr_t>(bestObj);
    if (m_assistJumpedFor == key) return std::nullopt;
    // Valid take-off window is roughly 8%-34% of a jump's length before the
    // spike; aim for the profile's preferred part of that window, per-spike.
    float frac = 0.08f + 0.26f * (0.45f + 0.20f * Humanizer::hash01(key)); // Mode 1: 45-65%
    switch (m_human.params().profile) {
        case Profile::Prodigy: frac = 0.08f + 0.26f * (0.35f + 0.20f * Humanizer::hash01(key)); break; // 35-55%
        case Profile::Unreal: frac = 0.08f + 0.26f * 0.5f; break; // no centering bias, no variance
        default: break;
    }
    if (best <= frac * span) {
        m_assistJumpedFor = key;
        m_assistHold[ch] = Humanizer::tapLength(m_human.params(), tick * 13 + ch);
        return true;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// main tick
// ---------------------------------------------------------------------------

void Pathfinder::onTick(PlayLayer* pl) {
    if (!m_inLevel) return;
    if (!active(pl)) {
        if (m_hold[0] || m_hold[1]) releaseAll(pl);
        m_fastForward = false;
        if (!m_enabled) m_status = "Off (F6 to start)";
        else if (!pl->m_isPracticeMode) m_status = "Enter Practice Mode to start";
        else if (pl->m_isPlatformer) m_status = "Platformer levels aren't supported";
        return;
    }
    auto tick = static_cast<uint64_t>(pl->m_gameState.m_currentProgress);
    if (tick == m_lastTick) return;
    m_lastTick = tick;
    m_tickCounter++;
    if (m_index.empty()) buildIndex(pl);
    if (m_segments.empty()) syncSegments(pl);
    refreshLive(pl, tick);

    // Checkpoints placed or removed by hand mid-attempt.
    size_t cps = pl->m_checkpointArray ? pl->m_checkpointArray->count() : 0;
    if (cps + 1 > m_segments.size()) onCheckpointAdded(pl, tick, m_expectOurs);
    while (m_segments.size() > cps + 1) m_segments.pop_back();

    if (!pl->m_player1 || pl->m_player1->m_isDead) {
        m_fastForward = false;
        return;
    }

    int nch = channels(pl);
    PlayerSnap s0;
    for (uint8_t ch = 0; ch < nch; ch++) {
        PlayerObject* p = ch == 0 ? pl->m_player1 : pl->m_player2;
        PlayerSnap s = snap(p, ch);
        if (!s.valid) continue;
        updateBounds(ch, s);
        {
            // Damped vertical speed: Mode 2 reacts to a smoothed curve, which
            // gives straight-fly a soft organic arc. Modes 1/3 use raw speed.
            float a = std::clamp(m_human.vyDamping(), 0.05f, 1.f);
            m_vySmooth[ch] = (a >= 1.f) ? s.vy : m_vySmooth[ch] + (s.vy - m_vySmooth[ch]) * a;
        }
        int orbType = touchingType(s, ObjKind::Orb);
        int padType = touchingType(s, ObjKind::Pad);
        bool orb = orbType >= 0;

        auto& sg = seg();

        // Replays of a known prefix must reproduce the attempt they came
        // from. If they don't, the level is random or something moved:
        // remember that, so the next death isn't blamed on the candidate.
        if (!m_divergedThisAttempt && sg.cluster && !sg.activePolish && tick < sg.cluster->windowStart &&
            m_havePrevY[ch]) {
            if (auto ref = sg.cluster->trace.at(ch, tick)) {
                if (std::abs(ref->x - s.x) > 2.f || std::abs(ref->y - s.y) > 3.f) m_divergedThisAttempt = true;
            }
        }

        if (tick < sg.lockedUntil) {
            press(pl, ch, planStateAt(sg, ch, tick), tick, false);
        } else if (auto want = livePolicy(pl, ch, s, tick)) {
            press(pl, ch, *want, tick, true);
        }

        // Learn the game's physics from what just happened.
        if (m_havePrevY[ch] && tick > 0) {
            if (auto prev = m_trace.at(ch, tick - 1)) {
                // Orb clicked / pad touched: learn what it does in this mode.
                if (prev->mode == s.mode && prev->mini == s.mini) {
                    float gNow = s.upsideDown ? -1.f : 1.f;
                    float gPrev = prev->upsideDown ? -1.f : 1.f;
                    float va = s.vy * gNow;
                    float vb = prev->vy * gPrev;
                    bool flipped = prev->upsideDown != s.upsideDown;
                    bool kicked = flipped || std::abs(va - vb) > 0.5f;
                    if (kicked && prev->orbType >= 0 && prev->hold) {
                        m_model.observeSpecial(prev->orbType, s.mode, s.mini, va);
                    } else if (kicked && prev->padType >= 0) {
                        m_model.observeSpecial(prev->padType, s.mode, s.mini, va);
                    }
                }
                if (prev->mode == s.mode && prev->mini == s.mini && prev->upsideDown == s.upsideDown &&
                    !prev->touchingOrb && !orb && prev->padType < 0 && padType < 0) {
                    int64_t since = static_cast<int64_t>(tick) - m_lastPress[ch];
                    int tsp = since > 1000 ? 1000 : static_cast<int>(since);
                    m_model.observe(s.mode, s.mini, s.upsideDown, prev->hold, prev->onGround, s.onGround, prev->vy,
                                    s.vy, s.vx, tsp);
                }
            }
        }

        auto& tr = m_trace.ticks[ch];
        if (tick >= m_trace.base) {
            auto idx = static_cast<size_t>(tick - m_trace.base);
            TraceTick tt;
            tt.x = s.x;
            tt.y = s.y;
            tt.vy = s.vy;
            tt.onGround = s.onGround;
            tt.hold = m_hold[ch];
            tt.touchingOrb = orb;
            tt.upsideDown = s.upsideDown;
            tt.mode = s.mode;
            tt.mini = s.mini;
            tt.w = s.rect.size.width;
            tt.h = s.rect.size.height;
            tt.orbType = orbType;
            tt.padType = padType;
            if (idx <= 200000) {
                while (tr.size() < idx) tr.push_back(tr.empty() ? tt : tr.back());
                if (tr.size() == idx) tr.push_back(tt);
            }
        }
        m_prevY[ch] = s.y;
        m_prevXs[ch] = s.x;
        m_havePrevY[ch] = true;
        if (ch == 0) s0 = s;
    }
    if (!s0.valid) return;

    auto& sg = seg();

    // Fast-forward through the part of this retry that is already solved.
    {
        uint64_t target = 0;
        if (sg.cluster && !sg.noFastForward) target = std::min(sg.cluster->windowStart, sg.lockedUntil);
        m_fastForward = tick + 40 < target;
    }

    // The spot being fixed was just cleared.
    if (sg.cluster && !sg.cluster->resolvedNoted && tick > sg.cluster->deathTick + 12) {
        notePolish(sg, *sg.cluster);
        m_status = fmt::format("Cleared the {:.0f}% spot", pl->getCurrentPercent());
    }

    // Just cleared a portal/orb/pad?
    if (m_prevX > 0.f && s0.x > m_prevX) {
        std::vector<WorldObject const*> objs;
        queryWindow(m_prevX, s0.x, objs);
        for (auto w : objs) {
            if (w->kind != ObjKind::Orb && w->kind != ObjKind::Portal && w->kind != ObjKind::Pad) continue;
            float mx = w->obj->getObjectRect().getMaxX();
            if (mx > m_prevX - 30.f && mx <= s0.x - 30.f) m_lastSpecialPass = tick;
        }
    }
    m_prevX = s0.x;

    if (!m_pendingCheckpoint && settingBool("auto-checkpoints")) {
        bool timeUp = tick - m_lastCheckpointTick >= m_cpInterval;
        bool cleared = m_lastSpecialPass && tick - m_lastSpecialPass <= 30 && tick - m_lastSpecialPass >= 6 &&
                       tick - m_lastCheckpointTick >= 72;
        if ((timeUp || cleared) && safeForCheckpoint(pl, s0, tick)) m_pendingCheckpoint = true;
    }
}

bool Pathfinder::isDoomedState(PlayerSnap const& s) const {
    for (auto const& d : m_doomed) {
        if (d.mode != s.mode || d.upsideDown != s.upsideDown) continue;
        if (std::abs(d.x - s.x) < 40.f && std::abs(d.y - s.y) < 18.f && std::abs(d.vy - s.vy) < 2.5f) return true;
    }
    return false;
}

bool Pathfinder::safeForCheckpoint(PlayLayer* pl, PlayerSnap const& s, uint64_t tick) const {
    auto const& sg = m_segments.back();
    if (sg.cluster && tick <= sg.cluster->deathTick + 12) return false; // not past the known trouble yet
    if (tick < sg.lockedUntil) return false;
    if (tick < sg.startTick + 60) return false;
    if (m_pendingToggleAt[0] >= 0 || m_tapReleaseAt[0] >= 0) return false;
    if (sg.activePolish) return false; // don't freeze an untested change into a checkpoint

    std::vector<WorldObject const*> objs;
    auto checkPlayer = [&](PlayerSnap const& p, uint8_t ch) {
        if (!p.valid || p.dead) return false;
        float half = p.rect.size.height * 0.5f;
        queryWindow(p.x, p.x + 60.f, objs);
        for (auto w : objs) {
            if (w->kind != ObjKind::Hazard) continue;
            auto const& r = w->obj->getObjectRect();
            if (r.getMinY() < p.y + half + 40.f && r.getMaxY() > p.y - half - 40.f) return false;
        }
        if (!isFlyMode(p.mode)) {
            if (!p.onGround || m_hold[ch]) return false;
        } else {
            Corridor c = corridor(pl, ch, p, 120.f);
            if (!c.valid) return false;
            float h = c.yMax - c.yMin;
            if (h < 30.f) return false;
            if (p.y < c.yMin + h * 0.25f || p.y > c.yMax - h * 0.25f) return false;
            if (std::abs(p.vy) > 3.f) return false;
        }
        return true;
    };
    if (!checkPlayer(s, 0)) return false;
    if (isDoomedState(s)) return false; // a checkpoint here already turned out to be a dead end
    if (pl->m_gameState.m_isDualMode && pl->m_player2) {
        uint8_t ch2 = channels(pl) == 2 ? 1 : 0;
        PlayerSnap s2 = snap(pl->m_player2, ch2);
        if (ch2 == 0) s2.vy = 0; // no separate history for p2 in single-channel dual
        if (!checkPlayer(s2, ch2)) return false;
    }
    if (sg.blacklist.count(planHash(sg.plan, tick))) return false;
    return true;
}

void Pathfinder::onPostUpdate(PlayLayer* pl) {
    if (!m_inLevel || !m_pendingCheckpoint) return;
    m_pendingCheckpoint = false;
    if (!active(pl) || !pl->m_player1 || pl->m_player1->m_isDead) return;
    if (!pl->m_checkpointArray) return;

    // Several ticks may have run since the spot was judged safe (high
    // speedhack, fast-forward, low FPS): judge the state as it is now.
    auto now = static_cast<uint64_t>(pl->m_gameState.m_currentProgress);
    PlayerSnap s = snap(pl->m_player1, 0);
    if (auto lt = m_trace.last(0)) s.vy = lt->vy;
    if (!safeForCheckpoint(pl, s, now)) return; // try again on a later tick

    size_t before = pl->m_checkpointArray->count();
    m_expectOurs = true;
    pl->markCheckpoint();
    size_t after = pl->m_checkpointArray->count();
    if (after > before && m_segments.size() < after + 1) onCheckpointAdded(pl, now, true);
    m_expectOurs = false;
}

// ---------------------------------------------------------------------------
// search
// ---------------------------------------------------------------------------

uint64_t Pathfinder::lookback(Mode m) const {
    switch (m) {
        case Mode::Cube: return 96;
        case Mode::Robot: return 110;
        case Mode::Ball: return 70;
        case Mode::Spider: return 60;
        case Mode::Ship: return 72;
        case Mode::Ufo: return 72;
        case Mode::Wave: return 48;
        case Mode::Swing: return 60;
    }
    return 90;
}

uint64_t Pathfinder::planHash(std::vector<Event> const& events, uint64_t uptoTick) const {
    uint64_t h = FNV_OFFSET;
    for (auto const& e : events) {
        if (e.tick >= uptoTick) continue;
        fnv(h, e.tick);
        fnv(h, (e.hold ? 1u : 0u) | (static_cast<uint64_t>(e.ch) << 1));
    }
    fnv(h, uptoTick / 6);
    return h;
}

uint64_t Pathfinder::candidateKey(std::vector<Event> const& base, Candidate const& c) const {
    uint64_t h = planHash(base, UINT64_MAX);
    fnv(h, 0xC0FFEEull);
    for (auto const& e : c.events) {
        fnv(h, e.tick);
        fnv(h, (e.hold ? 1u : 0u) | (static_cast<uint64_t>(e.ch) << 1));
    }
    if (c.live) {
        fnv(h, 0x11FEull);
        fnv(h, static_cast<uint64_t>(static_cast<int64_t>(std::lround(c.fly.centerOffset * 10.f))));
        fnv(h, static_cast<uint64_t>(std::lround(c.fly.bandScale * 100.f)));
        fnv(h, static_cast<uint64_t>(std::lround(c.fly.leadScale * 100.f)));
    }
    return h;
}

void Pathfinder::describe(Candidate& c, uint64_t windowStart) const {
    c.press = -1;
    c.hold = 0;
    for (auto const& e : c.events) {
        if (e.tick < windowStart) continue;
        if (e.hold && c.press < 0) {
            c.press = static_cast<int64_t>(e.tick);
        } else if (!e.hold && c.press >= 0 && c.hold == 0) {
            c.hold = static_cast<int>(static_cast<int64_t>(e.tick) - c.press);
        }
    }
}

bool Pathfinder::respectsCooldown(std::vector<Event> const& base, std::vector<Event> const& cand, uint8_t ch) const {
    int cd = profileSpec(m_profile).pressCooldown;
    if (cd <= 0) return true;
    std::vector<std::pair<uint64_t, bool>> presses; // tick, from candidate
    for (auto const& e : base) if (e.ch == ch && e.hold) presses.push_back({e.tick, false});
    for (auto const& e : cand) if (e.ch == ch && e.hold) presses.push_back({e.tick, true});
    std::sort(presses.begin(), presses.end());
    for (size_t i = 1; i < presses.size(); i++) {
        if (!presses[i].second && !presses[i - 1].second) continue; // old inputs aren't re-judged
        if (presses[i].first - presses[i - 1].first < static_cast<uint64_t>(cd)) return false;
    }
    return true;
}

void Pathfinder::rebuildBase(Segment& s, Cluster& c) {
    (void)s;
    c.base.clear();
    for (auto const& e : c.attemptPlan) {
        if (e.tick < c.windowStart || e.ch != c.ch) c.base.push_back(e);
    }
}

void Pathfinder::openCluster(Segment& s, uint64_t deathTick, uint8_t ch, Mode mode, int side, bool doomed,
                             Trace const& trace, std::vector<Event> const& attemptPlan) {
    Cluster c;
    c.deathTick = deathTick;
    c.bestDeath = deathTick;
    c.ch = ch;
    c.mode = mode;
    c.side = side;
    c.doomed = doomed;
    c.trace = trace;
    c.attemptPlan = attemptPlan;
    uint64_t L = lookback(mode);
    c.windowStart = std::max(s.startTick, deathTick > L ? deathTick - L : 0);
    rebuildBase(s, c);

    // What just died is not worth trying again.
    Candidate died;
    for (auto const& e : attemptPlan) {
        if (e.ch == ch && e.tick >= c.windowStart) died.events.push_back(e);
    }
    s.tried.insert(candidateKey(c.base, died));

    s.cluster = std::move(c);
    s.flyOverride.reset();
    buildQueue(s, *s.cluster);
}

void Pathfinder::buildQueue(Segment& s, Cluster& c) {
    uint64_t ws = c.windowStart;
    uint64_t d = std::max(c.deathTick, ws + 2);
    int floorT = s.human.baseFloor;
    bool unreal = s.human.profile == Profile::Unreal;
    bool holdAtWs = stateFrom(c.base, c.ch, ws, false);
    std::vector<Candidate> out;
    std::unordered_set<uint64_t> seen;

    auto add = [&](Candidate cand) {
        sortEvents(cand.events);
        cand.key = candidateKey(c.base, cand);
        if (s.tried.count(cand.key) || seen.count(cand.key)) return;
        if (!cand.live && !respectsCooldown(c.base, cand.events, c.ch)) return;
        describe(cand, ws);
        seen.insert(cand.key);
        out.push_back(std::move(cand));
    };
    auto withRelease = [&](std::vector<Event> ev) {
        if (holdAtWs) ev.insert(ev.begin(), Event{ws, false, c.ch});
        return ev;
    };

    // (1) zero input through the window
    {
        Candidate z;
        z.events = withRelease({});
        z.score = 0;
        add(z);
    }

    // Flying: first try steering the corridor controller differently.
    if (isFlyMode(c.mode)) {
        int first = c.side > 0 ? -1 : 1; // killed by something above -> fly lower
        std::vector<std::pair<float, float>> tweaks = {{1.f, 1.f}, {0.6f, 1.f}, {1.f, 1.6f}, {0.6f, 1.6f}};
        double sc = 5;
        for (auto [bandS, leadS] : {std::pair{0.6f, 1.f}, std::pair{1.f, 1.6f}}) {
            Candidate lc;
            lc.live = true;
            lc.fly = {0.f, bandS, leadS};
            lc.score = sc++;
            add(lc);
        }
        for (float off : {12.f, 24.f, 36.f, 54.f}) {
            for (int sign : {first, -first}) {
                for (auto [bandS, leadS] : tweaks) {
                    Candidate lc;
                    lc.live = true;
                    lc.fly = {off * static_cast<float>(sign), bandS, leadS};
                    lc.score = sc++;
                    add(lc);
                }
            }
        }
    }

    // (2) single clicks, anchored where a person would click.
    struct Anchor { uint64_t tick; double score; };
    std::vector<Anchor> anchors;
    double groupBase = isFlyMode(c.mode) ? 100.0 : 10.0;

    // Orb activation windows seen in the attempt that died.
    {
        uint64_t runStart = 0;
        bool inRun = false;
        int group = 0;
        for (uint64_t t = ws; t <= d; t++) {
            auto tt = c.trace.at(c.ch, t);
            bool on = tt && tt->touchingOrb && t < d;
            if (on && !inRun) { inRun = true; runStart = t; }
            if (!on && inRun) {
                inRun = false;
                auto order = Humanizer::orderWindow(s.human, runStart, t - 1, 1, runStart);
                for (size_t i = 0; i < order.size(); i++) anchors.push_back({order[i], groupBase + group * 2 + i * 1.0});
                group++;
            }
        }
    }
    // Platform edges: last ground ticks before walking off.
    {
        for (uint64_t t = ws; t + 1 < d; t++) {
            auto a = c.trace.at(c.ch, t);
            auto b = c.trace.at(c.ch, t + 1);
            if (!a || !b) continue;
            if (a->onGround && !b->onGround && !a->hold && !b->hold) {
                uint64_t lo = t > ws + 18 ? t - 18 : ws;
                auto order = Humanizer::orderWindow(s.human, lo, t, 1, lo);
                for (size_t i = 0; i < order.size(); i++) anchors.push_back({order[i], groupBase + 20 + i * 1.0});
            }
        }
    }
    // Generic sweep around the usual reaction distance before the death.
    {
        uint64_t lead = 30;
        switch (c.mode) {
            case Mode::Cube: lead = 32; break;
            case Mode::Robot: lead = 36; break;
            case Mode::Ball: lead = 18; break;
            case Mode::Spider: lead = 14; break;
            default: lead = 20; break;
        }
        uint64_t ideal = d > lead ? d - lead : ws;
        ideal = std::clamp(ideal, ws, d - 1);
        std::vector<uint64_t> ticks;
        uint64_t step = unreal ? 1 : 2; // Mode 3 sweeps every tick
        for (uint64_t t = ws; t + 1 < d; t += step) ticks.push_back(t);
        std::stable_sort(ticks.begin(), ticks.end(), [&](uint64_t a, uint64_t b) {
            auto da = a > ideal ? a - ideal : ideal - a;
            auto db = b > ideal ? b - ideal : ideal - b;
            return da < db;
        });
        for (size_t i = 0; i < ticks.size(); i++) anchors.push_back({ticks[i], groupBase + 40 + i * 1.0});
    }

    // Hold lengths. Where the hold length doesn't change the physics (cube
    // taps, ball/spider/UFO/swing clicks) the first length is a natural,
    // varied tap instead of always the exact minimum.
    auto lengthsFor = [&](uint64_t anchorTick) {
        std::vector<int> lengths;
        int tap = std::max(floorT, Humanizer::tapLength(s.human, anchorTick * 131 + c.deathTick));
        if (unreal) {
            switch (c.mode) {
                case Mode::Cube: lengths = {1, 12, 44}; break;
                case Mode::Robot: lengths = {1, 24, 34, 48}; break;
                case Mode::Ship:
                case Mode::Wave: lengths = {1, 8, 26, 44}; break;
                default: lengths = {1, 8}; break;
            }
        } else {
            switch (c.mode) {
                case Mode::Cube: lengths = {tap, floorT + 12, 44}; break;
                case Mode::Robot: lengths = {floorT, 24, 34, 48}; break;
                case Mode::Ship:
                case Mode::Wave: lengths = {floorT, 26, 44}; break;
                default: lengths = {tap}; break;
            }
        }
        return lengths;
    };

    std::unordered_set<uint64_t> usedTick;
    for (auto const& a : anchors) {
        if (holdAtWs && a.tick < ws + static_cast<uint64_t>(floorT)) continue;
        if (!usedTick.insert(a.tick).second) continue;
        auto lengths = lengthsFor(a.tick);
        for (size_t li = 0; li < lengths.size(); li++) {
            Candidate cand;
            cand.events = withRelease({{a.tick, true, c.ch}, {a.tick + static_cast<uint64_t>(lengths[li]), false, c.ch}});
            cand.score = a.score + static_cast<double>(li) * 150.0;
            add(cand);
        }
        if (holdAtWs) {
            // "keep holding, then let go here"
            Candidate keep;
            keep.events = {{a.tick, false, c.ch}};
            keep.score = a.score + 5.0;
            add(keep);
        }
    }

    // Anti-robot filter: push candidates whose resulting click rhythm is
    // machine-regular behind otherwise-equal ones. (Off in Mode 3: exact play.)
    if (!unreal) {
        for (auto& cand : out) {
            std::vector<uint64_t> ticks;
            for (auto const& e : c.base) if (e.ch == c.ch && e.tick + 400 >= ws) ticks.push_back(e.tick);
            for (auto const& e : cand.events) ticks.push_back(e.tick);
            std::sort(ticks.begin(), ticks.end());
            cand.score += Humanizer::periodicity(ticks) * 25.0;
        }
    }
    std::stable_sort(out.begin(), out.end(), [](Candidate const& a, Candidate const& b) { return a.score < b.score; });

    if (out.size() > 400) out.resize(400);
    c.queue = std::move(out);
    c.next = 0;
    rankWithBrain(s, c);
    reprioritize(c);
}

void Pathfinder::addPairs(Segment& s, Cluster& c, bool front) {
    c.pairsAdded = true;
    int floorT = s.human.baseFloor;
    std::vector<Candidate const*> singles;
    // Best-performing singles first (progress-guided), then queue order.
    std::vector<Candidate> pool(c.queue.begin(), c.queue.end());
    if (c.bestPress >= 0) {
        std::stable_sort(pool.begin(), pool.end(), [&](Candidate const& a, Candidate const& b) {
            auto da = a.press < 0 ? INT64_MAX : absDiff(a.press, c.bestPress);
            auto db = b.press < 0 ? INT64_MAX : absDiff(b.press, c.bestPress);
            return da < db;
        });
    }
    for (auto const& cand : pool) {
        if (cand.live || cand.press < 0) continue;
        singles.push_back(&cand);
        if (singles.size() >= 10) break;
    }
    std::vector<Candidate> pairs;
    double sc = 500;
    for (auto const* first : singles) {
        uint64_t rel = 0;
        for (auto const& e : first->events) if (!e.hold) rel = std::max(rel, e.tick);
        for (int gap : {floorT, floorT + 10, 28, 44}) {
            uint64_t t2 = rel + static_cast<uint64_t>(gap);
            if (t2 > c.deathTick + 10) continue;
            Candidate p = *first;
            int tap = std::max(floorT, Humanizer::tapLength(s.human, t2 * 17 + c.deathTick));
            if (s.human.profile == Profile::Unreal) tap = 1;
            p.events.push_back({t2, true, c.ch});
            p.events.push_back({t2 + static_cast<uint64_t>(tap), false, c.ch});
            sortEvents(p.events);
            p.key = candidateKey(c.base, p);
            p.score = sc++;
            describe(p, c.windowStart);
            if (!s.tried.count(p.key) && respectsCooldown(c.base, p.events, c.ch)) pairs.push_back(std::move(p));
        }
    }
    if (front) {
        c.queue.insert(c.queue.begin() + static_cast<std::ptrdiff_t>(c.next), pairs.begin(), pairs.end());
    } else {
        c.queue.insert(c.queue.end(), pairs.begin(), pairs.end());
    }
}

void Pathfinder::addMicro(Segment& s, Cluster& c) {
    c.microAdded = true;
    if (s.human.profile == Profile::Prodigy) return; // crisp 6-8 tick inputs only
    int micro = s.human.microFloor;
    uint64_t ws = c.windowStart;
    uint64_t d = std::max(c.deathTick, ws + 2);
    bool holdAtWs = stateFrom(c.base, c.ch, ws, false);
    std::vector<Candidate> extra;
    if (isFlyMode(c.mode)) {
        for (float off : {0.f, 10.f, -10.f}) {
            Candidate lc;
            lc.live = true;
            lc.fly = {off, 0.35f, 1.2f};
            lc.key = candidateKey(c.base, lc);
            if (!s.tried.count(lc.key)) extra.push_back(lc);
        }
    }
    uint64_t ideal = c.bestPress >= 0 ? static_cast<uint64_t>(c.bestPress) : (d > 20 ? d - 20 : ws);
    for (int off = 0; off <= 30; off++) {
        for (int sign : {1, -1}) {
            if (off == 0 && sign < 0) continue;
            int64_t t = static_cast<int64_t>(ideal) + off * sign;
            if (t < static_cast<int64_t>(ws) || t + 1 >= static_cast<int64_t>(d)) continue;
            Candidate cand;
            if (holdAtWs) {
                // micro-release while holding
                cand.events = {{static_cast<uint64_t>(t), false, c.ch}, {static_cast<uint64_t>(t + micro), true, c.ch}};
            } else {
                cand.events = {{static_cast<uint64_t>(t), true, c.ch}, {static_cast<uint64_t>(t + micro), false, c.ch}};
            }
            cand.key = candidateKey(c.base, cand);
            describe(cand, ws);
            if (!s.tried.count(cand.key) && respectsCooldown(c.base, cand.events, c.ch)) extra.push_back(std::move(cand));
        }
    }
    // Interleave with what is left so micro inputs get a fair chance now.
    std::vector<Candidate> rest(c.queue.begin() + static_cast<std::ptrdiff_t>(c.next), c.queue.end());
    c.queue.resize(c.next);
    size_t i = 0, j = 0;
    while (i < rest.size() || j < extra.size()) {
        if (j < extra.size()) c.queue.push_back(std::move(extra[j++]));
        if (i < rest.size()) c.queue.push_back(std::move(rest[i++]));
    }
}

void Pathfinder::addSweep(Segment& s, Cluster& c) {
    c.sweepAdded = true;
    uint64_t ws = c.windowStart;
    uint64_t d = std::max(c.deathTick, ws + 2);
    int64_t anchor = c.bestPress >= 0 ? c.bestPress : static_cast<int64_t>(d > 20 ? d - 20 : ws);
    int sustained = c.bestHold > 1 ? c.bestHold : 12;
    bool holdAtWs = stateFrom(c.base, c.ch, ws, false);

    std::vector<Candidate> sweep;
    for (int off = -8; off <= 8; off++) {
        int64_t t = anchor + off;
        if (t < static_cast<int64_t>(ws) || t > static_cast<int64_t>(d) + 8) continue;
        auto tt = static_cast<uint64_t>(t);
        std::vector<std::vector<Event>> variants = {
            {{tt, true, c.ch}, {tt + 1, false, c.ch}},                                // 1-tick swift
            {{tt, true, c.ch}, {tt + static_cast<uint64_t>(sustained), false, c.ch}}, // sustained hold
        };
        if (holdAtWs) variants.push_back({{tt, false, c.ch}}); // release here
        for (auto& v : variants) {
            Candidate cand;
            if (holdAtWs && v.size() > 1) cand.events.push_back({ws, false, c.ch});
            for (auto const& e : v) cand.events.push_back(e);
            sortEvents(cand.events);
            cand.key = candidateKey(c.base, cand);
            describe(cand, ws);
            if (s.tried.count(cand.key) || !respectsCooldown(c.base, cand.events, c.ch)) continue;
            sweep.push_back(std::move(cand));
        }
    }
    c.queue.insert(c.queue.begin() + static_cast<std::ptrdiff_t>(c.next), sweep.begin(), sweep.end());
    m_status = fmt::format("Frame sweep around tick {} ({} tries)", anchor, sweep.size());
}

void Pathfinder::reprioritize(Cluster& c) {
    // Progress-guided search: if some click got further than the original
    // death, try the untested clicks closest to it next, on the side whose
    // tested neighbour did better.
    TryResult const* best = nullptr;
    for (auto const& r : c.results) {
        if (r.press < 0) continue;
        if (!best || r.outcome > best->outcome) best = &r;
    }
    if (!best || best->outcome <= c.deathTick) return;
    c.bestPress = best->press;
    c.bestHold = best->hold;

    uint64_t below = 0, above = 0; // outcome of the nearest tested neighbour on each side
    int64_t nearBelow = INT64_MIN, nearAbove = INT64_MAX;
    for (auto const& r : c.results) {
        if (r.press < 0 || std::abs(r.hold - best->hold) > 2) continue;
        if (r.press < best->press && r.press > nearBelow) { nearBelow = r.press; below = r.outcome; }
        if (r.press > best->press && r.press < nearAbove) { nearAbove = r.press; above = r.outcome; }
    }
    if (nearBelow == INT64_MIN) below = best->outcome; // unexplored side: neutral
    if (nearAbove == INT64_MAX) above = best->outcome;
    int preferSide = above >= below ? 1 : -1;

    std::vector<Candidate> nearList, farList;
    for (size_t i = c.next; i < c.queue.size(); i++) {
        auto& cand = c.queue[i];
        if (!cand.live && cand.press >= 0 && absDiff(cand.press, best->press) <= 12) nearList.push_back(std::move(cand));
        else farList.push_back(std::move(cand));
    }
    std::stable_sort(nearList.begin(), nearList.end(), [&](Candidate const& a, Candidate const& b) {
        auto score = [&](Candidate const& x) {
            double dist = static_cast<double>(absDiff(x.press, best->press));
            double side = (x.press - best->press) * preferSide >= 0 ? 0.0 : 0.5;
            double hold = std::abs(x.hold - best->hold) * 0.25;
            return dist + side + hold;
        };
        return score(a) < score(b);
    });
    c.queue.resize(c.next);
    for (auto& x : nearList) c.queue.push_back(std::move(x));
    for (auto& x : farList) c.queue.push_back(std::move(x));
}

void Pathfinder::widenCluster(Segment& s, Cluster& c) {
    c.widen++;
    uint64_t L = lookback(c.mode) / 2;
    c.windowStart = std::max(s.startTick, c.windowStart > L ? c.windowStart - L : 0);
    rebuildBase(s, c);
    c.pairsAdded = c.microAdded = c.sweepAdded = false;
    buildQueue(s, c);
}

bool Pathfinder::escalate(PlayLayer* pl, Segment& s, Cluster& c) {
    // Stuck detector: no progress for a while -> change approach now
    // instead of grinding through the rest of the queue.
    c.triesSinceImprove = 0;
    c.escalation++;
    if (c.escalation == 1 && !c.pairsAdded) {
        addPairs(s, c, true);
        m_status = "Stuck: trying two-click setups";
        return false;
    }
    if (c.escalation <= 2 && c.widen < 2) {
        widenCluster(s, c);
        m_status = "Stuck: changing inputs further back";
        return false;
    }
    // Dying close to the checkpoint without progress: the checkpoint itself
    // is the problem.
    if (m_segments.size() > 1 && c.deathTick < s.startTick + 2 * lookback(c.mode)) {
        backtrack(pl);
        return true;
    }
    if (c.widen < 2) widenCluster(s, c);
    return false;
}

void Pathfinder::applyCandidate(Segment& s, Cluster& c, Candidate const& cand) {
    c.current = cand;
    c.hasCurrent = true;
    s.plan = c.base;
    for (auto const& e : cand.events) s.plan.push_back(e);
    sortEvents(s.plan);
    if (cand.live) {
        s.lockedUntil = c.windowStart;
        s.flyOverride = FlyOverride{c.windowStart, c.deathTick + 90, c.ch, cand.fly};
    } else {
        uint64_t last = c.deathTick;
        for (auto const& e : cand.events) last = std::max(last, e.tick);
        s.lockedUntil = last + 1;
        s.flyOverride.reset();
    }
}

bool Pathfinder::applyNextCandidate(Segment& s) {
    if (!s.cluster) return false;
    auto& c = *s.cluster;
    if (c.tries > 15 && !c.microAdded) addMicro(s, c);

    for (int guard = 0; guard < 8; guard++) {
        while (c.next < c.queue.size()) {
            Candidate cand = c.queue[c.next++];
            if (s.tried.count(cand.key)) continue;
            s.tried.insert(cand.key);
            applyCandidate(s, c, cand);
            m_status = fmt::format("{} @ tick {}: try {} ({})", c.doomed ? "Re-routing" : "Fixing", c.deathTick,
                                   c.tries + 1, cand.live ? "steer" : (cand.events.empty() ? "no input" : "clicks"));
            return true;
        }
        if (!c.pairsAdded) { addPairs(s, c, false); continue; }
        if (!c.microAdded) { addMicro(s, c); continue; }
        if (c.widen < 2) { widenCluster(s, c); continue; }
        break;
    }
    return false;
}

void Pathfinder::restartStuck(Segment& s) {
    // Nothing left to try and nowhere to backtrack to: start this spot over
    // with a much wider window and setups first, so the next round isn't a
    // repeat of the last one.
    if (!s.cluster) return;
    auto c = *s.cluster;
    s.rootRounds++;
    s.failures = 0;
    s.tried.clear();
    openCluster(s, c.deathTick, c.ch, c.mode, c.side, c.doomed, c.trace, c.attemptPlan);
    auto& nc = *s.cluster;
    uint64_t L = lookback(nc.mode) * static_cast<uint64_t>(1 + std::min(s.rootRounds, 3));
    nc.windowStart = std::max(s.startTick, nc.deathTick > L ? nc.deathTick - L : 0);
    nc.widen = 2;
    rebuildBase(s, nc);
    buildQueue(s, nc);
    addPairs(s, nc, true);
    applyNextCandidate(s);
    m_status = s.rootRounds >= 2
                   ? fmt::format("Stuck at tick {} (round {}): try Mode 3 (F7) or a checkpoint earlier", nc.deathTick,
                                 s.rootRounds + 1)
                   : fmt::format("Retrying tick {} with a wider search", nc.deathTick);
}

void Pathfinder::backtrack(PlayLayer* pl) {
    for (int depth = 0; depth < 4 && m_segments.size() > 1; depth++) {
        auto& child = m_segments.back();
        uint64_t childStart = child.startTick;
        if (child.hasStartState) {
            auto const& st = child.startState;
            m_doomed.push_back({st.x, st.y, st.vy, st.mode, st.upsideDown});
        }
        size_t before = pl->m_checkpointArray ? pl->m_checkpointArray->count() : 0;
        pl->removeCheckpoint(false); // false = the most recent one
        size_t after = pl->m_checkpointArray ? pl->m_checkpointArray->count() : 0;
        if (after >= before) {
            log::warn("Pathfinder: removeCheckpoint did not remove anything");
            return;
        }
        m_segments.pop_back();
        auto& parent = seg();
        parent.blacklist.insert(planHash(parent.plan, childStart));
        parent.failures++;
        parent.activePolish.reset();
        parent.pendingPolish.reset();
        Mode mode = Mode::Cube;
        if (auto tt = parent.lastTrace.at(0, childStart > 0 ? childStart - 1 : 0)) mode = tt->mode;
        openCluster(parent, childStart, 0, mode, 0, true, parent.lastTrace, parent.plan);
        if (auto tt = parent.lastTrace.at(0, childStart > 0 ? childStart - 1 : 0)) parent.cluster->deathX = tt->x;
        m_status = fmt::format("Backtracked: checkpoint {} was a dead end", m_segments.size());
        if (parent.failures <= maxFailures() && applyNextCandidate(parent)) return;
    }
    if (m_segments.size() == 1) restartStuck(seg());
}

// ---------------------------------------------------------------------------
// polish pass: move solved clicks to a natural spot inside their window
// ---------------------------------------------------------------------------

void Pathfinder::notePolish(Segment& s, Cluster& c) {
    c.resolvedNoted = true;
    if (!settingBool("polish-pass")) return;
    if (!profileSpec(s.human.profile).centered) return; // Mode 3: exact timing
    if (!c.hasCurrent || c.current.live || c.current.press < 0 || c.current.hold <= 0) return;
    auto tw = static_cast<uint64_t>(c.current.press);
    int hold = c.current.hold;

    // The window this click works in, from the clicks that didn't.
    int64_t lo = -1, hi = -1;
    for (auto const& r : c.results) {
        if (r.press < 0 || std::abs(r.hold - hold) > 2) continue;
        if (r.press < static_cast<int64_t>(tw)) lo = std::max(lo, r.press);
        if (r.press > static_cast<int64_t>(tw)) hi = hi < 0 ? r.press : std::min(hi, r.press);
    }
    uint64_t wlo = lo >= 0 ? static_cast<uint64_t>(lo + 1) : (tw > 3 ? tw - 3 : 0);
    uint64_t whi = hi >= 0 ? static_cast<uint64_t>(hi - 1) : tw + 3;
    wlo = std::max(wlo, c.windowStart);
    if (whi <= wlo) return;
    uint64_t alt = Humanizer::humanSpot(s.human, wlo, whi, tw * 31 + 7);
    if (alt > whi || alt == tw) return;
    s.pendingPolish = PolishItem{tw, alt, hold, c.ch, c.deathTick};
}

bool Pathfinder::movePress(std::vector<Event>& events, uint8_t ch, uint64_t from, uint64_t to, int hold, int floorT,
                           bool check) const {
    auto pressIt = std::find_if(events.begin(), events.end(),
                                [&](Event const& e) { return e.ch == ch && e.hold && e.tick == from; });
    if (pressIt == events.end()) return false;
    uint64_t relFrom = from + static_cast<uint64_t>(hold);
    auto relIt = std::find_if(events.begin(), events.end(),
                              [&](Event const& e) { return e.ch == ch && !e.hold && e.tick == relFrom; });
    if (relIt == events.end()) return false;
    uint64_t relTo = to + static_cast<uint64_t>(hold);
    if (check) {
        int cd = profileSpec(m_profile).pressCooldown;
        for (auto const& e : events) {
            if (e.ch != ch || &e == &*pressIt || &e == &*relIt) continue;
            if (e.tick < from) {
                if (e.tick + static_cast<uint64_t>(floorT) > to) return false;
                if (e.hold && cd > 0 && e.tick + static_cast<uint64_t>(cd) > to) return false;
            } else {
                if (relTo + static_cast<uint64_t>(floorT) > e.tick) return false;
                if (e.hold && cd > 0 && to + static_cast<uint64_t>(cd) > e.tick) return false;
            }
        }
    }
    pressIt->tick = to;
    relIt->tick = relTo;
    sortEvents(events);
    return true;
}

void Pathfinder::startPolish(Segment& s) {
    if (!s.pendingPolish || s.activePolish || !s.cluster) return;
    auto p = *s.pendingPolish;
    auto& c = *s.cluster;
    // Only clicks that are settled, well before the spot being fixed now.
    uint64_t lastTick = std::max(p.orig, p.alt) + static_cast<uint64_t>(p.hold) + 20;
    if (lastTick >= c.windowStart) return; // keep it pending for a later spot
    s.pendingPolish.reset();
    int floorT = s.human.baseFloor;
    if (!movePress(s.plan, p.ch, p.orig, p.alt, p.hold, floorT, true)) return;
    movePress(c.base, p.ch, p.orig, p.alt, p.hold, floorT, false);
    movePress(c.attemptPlan, p.ch, p.orig, p.alt, p.hold, floorT, false);
    s.activePolish = p;
}

bool Pathfinder::resolvePolish(Segment& s, uint64_t deathTick) {
    if (!s.activePolish) return false;
    auto p = *s.activePolish;
    s.activePolish.reset();
    if (deathTick > p.ref + 6) {
        // Survived the polished spot: keep it. The cluster's reference trace
        // is from before the change, so refresh it.
        if (s.cluster) s.cluster->trace = m_trace;
        return false;
    }
    // Died at (or before) the spot the click got us past: put it back.
    int floorT = s.human.baseFloor;
    movePress(s.plan, p.ch, p.alt, p.orig, p.hold, floorT, false);
    if (s.cluster) {
        movePress(s.cluster->base, p.ch, p.alt, p.orig, p.hold, floorT, false);
        movePress(s.cluster->attemptPlan, p.ch, p.alt, p.orig, p.hold, floorT, false);
    }
    return true;
}

// ---------------------------------------------------------------------------
// deaths
// ---------------------------------------------------------------------------

void Pathfinder::onDeath(PlayLayer* pl, PlayerObject* player, GameObject* killer) {
    if (!m_inLevel || !active(pl) || m_deathHandled) return;
    m_deathHandled = true;
    m_totalDeaths++;
    if (m_totalDeaths % 50 == 0) saveModel();
    m_fastForward = false;
    if (settingBool("instant-respawn") && !m_fastRespawnBroken) {
        m_respawnPending = true;
        m_respawnFrames = 0;
    }
    if (m_segments.empty()) syncSegments(pl);

    auto tick = static_cast<uint64_t>(pl->m_gameState.m_currentProgress);
    uint8_t ch = (player == pl->m_player2 && channels(pl) == 2) ? 1 : 0;
    PlayerSnap ps = snap(player, ch);
    releaseAll(pl);
    m_pendingCheckpoint = false;

    auto& s = seg();

    // A natural-timing nudge being tested caused this: undo it, rerun.
    if (resolvePolish(s, tick)) {
        m_status = "Natural timing didn't hold here; kept the exact one";
        return;
    }

    s.failures++;

    int side = 0;
    if (killer && killer != pl->m_anticheatSpike) {
        side = killer->getObjectRect().getMidY() > ps.y ? 1 : -1;
    } else if (isFlyMode(ps.mode) && ch <= 1) {
        side = (m_modeCeil[ch] - ps.y) < (ps.y - m_modeFloor[ch]) ? 1 : -1;
    }
    Mode mode = ps.mode;
    if (auto tt = m_trace.at(ch, tick > 3 ? tick - 3 : 0)) mode = tt->mode;

    // The replay of the known part didn't match last time: the level is
    // random or our view of it was off. Retry the same candidate instead of
    // blaming it.
    if (m_divergedThisAttempt && s.cluster && s.cluster->hasCurrent) {
        s.divergences++;
        if (s.divergences >= 3) s.noFastForward = true;
        if (s.divergences <= 8) {
            s.failures--;
            applyCandidate(s, *s.cluster, s.cluster->current);
            m_status = "Replay drifted (random/moving part): retrying the same inputs";
            return;
        }
    }

    if (s.failures > maxFailures()) {
        if (m_segments.size() > 1) {
            backtrack(pl);
            return;
        }
        restartStuck(s);
        return;
    }

    if (s.cluster) {
        auto& c = *s.cluster;
        bool sameSpot = tick + 6 >= c.deathTick && tick <= c.deathTick + 6;
        bool worseInWindow = tick < c.deathTick && tick >= c.windowStart;
        if (sameSpot || worseInWindow) {
            c.tries++;
            if (c.hasCurrent) c.results.push_back({c.current.press, c.current.hold, tick});
            if (tick > c.bestDeath) {
                c.bestDeath = tick;
                c.triesSinceImprove = 0;
                if (c.hasCurrent && c.current.press >= 0) {
                    c.bestPress = c.current.press;
                    c.bestHold = c.current.hold;
                }
            } else {
                c.triesSinceImprove++;
            }
            if (std::abs(ps.x - c.deathX) <= 5.f) c.nearCrashes++;
            reprioritize(c);
            // Mode 3: stuck on the same obstacle >10 times -> exact frame sweep.
            if (profileSpec(m_profile).frameSweep && c.nearCrashes > 10 && !c.sweepAdded) addSweep(s, c);
            int stuckLimit = profileSpec(m_profile).frameSweep ? 16 : 10;
            if (c.triesSinceImprove >= stuckLimit) {
                if (escalate(pl, s, c)) return; // backtracked
            }
            if (!applyNextCandidate(s)) {
                if (m_segments.size() > 1) backtrack(pl);
                else restartStuck(s);
            }
            return;
        }
        if (!c.resolvedNoted && tick > c.deathTick + 6) notePolish(s, c);
    }
    openCluster(s, tick, ch, mode, side, false, m_trace, s.plan);
    s.cluster->deathX = ps.x;
    s.cluster->nearCrashes = 1;
    if (!applyNextCandidate(s)) {
        if (m_segments.size() > 1) backtrack(pl);
        else restartStuck(s);
        return;
    }
    startPolish(s);
}


// ---------------------------------------------------------------------------
// brain: learned physics + lookahead
// ---------------------------------------------------------------------------

SimState Pathfinder::toSim(PlayerSnap const& s, bool hold) const {
    SimState st;
    st.x = s.x;
    st.y = s.y;
    st.vy = s.vy;
    st.vx = s.vx;
    st.w = s.rect.size.width > 0.f ? s.rect.size.width : 30.f * s.size;
    st.h = s.rect.size.height > 0.f ? s.rect.size.height : 30.f * s.size;
    st.grounded = s.onGround;
    st.upside = s.upsideDown;
    st.mini = s.mini;
    st.hold = hold;
    st.mode = s.mode;
    return st;
}

SimWorld Pathfinder::simWorld(uint8_t ch, SimState const& st, float range) const {
    SimWorld w;
    std::vector<WorldObject const*> objs;
    queryWindow(st.x - st.w - 10.f, st.x + range, objs);
    w.objs.reserve(objs.size());
    for (auto o : objs) {
        SimObj so;
        so.r = o->obj->getObjectRect();
        so.kind = o->kind;
        so.slope = o->obj->m_objectType == GameObjectType::Slope;
        so.type = static_cast<int>(o->obj->m_objectType);
        so.speed = speedPortalSpeed(o->obj->m_objectID) / 240.f;
        w.objs.push_back(so);
    }
    std::sort(w.objs.begin(), w.objs.end(), [](SimObj const& a, SimObj const& b) { return a.r.getMinX() < b.r.getMinX(); });
    if (isBoundedMode(st.mode) && ch <= 1) {
        float floorY = m_modeFloor[ch];
        float ceilY = m_modeCeil[ch];
        int b = bucketOf(st.x);
        for (int d = -1; d <= 2; d++) {
            auto f = m_learnedFloor[ch].find(b + d);
            if (f != m_learnedFloor[ch].end()) floorY = std::max(floorY, f->second);
            auto k = m_learnedCeil[ch].find(b + d);
            if (k != m_learnedCeil[ch].end()) ceilY = std::min(ceilY, k->second);
        }
        // Stale guesses that don't contain the player are useless.
        if (st.y - st.h * 0.5f < floorY - 2.f) floorY = 90.f;
        if (st.y + st.h * 0.5f > ceilY + 2.f) ceilY = 1e9f;
        w.floorY = floorY;
        w.ceilY = ceilY;
    } else {
        w.floorY = 90.f;
        w.ceilY = 1e9f;
    }
    return w;
}

std::optional<bool> Pathfinder::brainGround(PlayLayer* pl, uint8_t ch, PlayerSnap const& s, uint64_t tick) {
    (void)pl;
    int floorT = m_human.minStateTicks(spotFailures(tick));
    auto& plan = m_brainPlan[ch];
    if (m_hold[ch]) {
        int len = std::max(floorT, m_assistHold[ch]);
        if (tick - m_lastToggle[ch] >= static_cast<uint64_t>(len)) {
            m_assistHold[ch] = 0;
            return false;
        }
        return std::nullopt;
    }
    // Planning also runs in the air: a press just before landing (held
    // into the landing) or a click on an orb ahead is decided by the
    // simulation, like a player pressing slightly early.
    if (s.vx <= 0.3f) {
        plan = {};
        return std::nullopt;
    }

    auto fire = [&]() -> std::optional<bool> {
        m_assistHold[ch] = std::max(floorT, plan.len);
        plan = {};
        return true;
    };

    // A recent plan is still good: click when its moment comes.
    if (plan.target >= 0 && static_cast<int64_t>(tick) <= plan.hi && tick - plan.madeAt < 8) {
        if (static_cast<int64_t>(tick) >= plan.target) return fire();
        return std::nullopt;
    }
    plan = {};

    SimState st = toSim(s, false);
    SimWorld w = simWorld(ch, st, st.vx * 140.f + 80.f);
    InputFn none = [](int) { return false; };
    auto rNone = Simulator::run(m_model, st, w, none, 120);
    if (!rNone.died) return std::nullopt; // doing nothing is safe: do nothing

    int since = static_cast<int>(tick - m_lastToggle[ch]);
    int64_t earliest = std::max(0, floorT - since);
    int cd = m_human.pressCooldown();
    if (cd > 0) earliest = std::max<int64_t>(earliest, m_lastPress[ch] + cd - static_cast<int64_t>(tick));
    int K = std::min(rNone.ticks - 1, 90);
    if (earliest > K) return std::nullopt;

    int tap = std::max(floorT, Humanizer::tapLength(m_human.params(), tick * 13 + ch));
    if (m_human.params().profile == Profile::Unreal) tap = std::max(1, floorT);
    std::vector<int> lens = {tap};
    if (s.mode == Mode::Robot) lens = {tap, 24, 36, 50};
    else if (s.mode == Mode::Cube) lens.push_back(std::max(floorT, 40));

    for (int len : lens) {
        auto survives = [&](int k) {
            InputFn fn = [k, len](int i) { return i >= k && i < k + len; };
            return !Simulator::run(m_model, st, w, fn, k + 110).died;
        };
        int first = -1;
        for (int k = static_cast<int>(earliest); k <= K; k += 2) {
            if (survives(k)) { first = k; break; }
        }
        if (first < 0) continue;
        int lo = first;
        while (lo - 1 >= earliest && survives(lo - 1)) lo--;
        int hi = first;
        while (hi + 1 <= K && hi - lo < 60 && survives(hi + 1)) hi++;
        plan.lo = static_cast<int64_t>(tick) + lo;
        plan.hi = static_cast<int64_t>(tick) + hi;
        plan.len = len;
        plan.madeAt = tick;
        // Click where this profile likes to inside the window that works.
        auto spot = Humanizer::humanSpot(m_human.params(), static_cast<uint64_t>(plan.lo), static_cast<uint64_t>(plan.hi),
                                         static_cast<uint64_t>(plan.hi) * 7 + ch * 3);
        if (spot > static_cast<uint64_t>(plan.hi)) spot = static_cast<uint64_t>(plan.lo + (plan.hi - plan.lo) / 2);
        plan.target = static_cast<int64_t>(spot);
        break;
    }
    if (plan.target < 0) return std::nullopt; // no click helps: let the search learn from it
    if (static_cast<int64_t>(tick) >= plan.target) return fire();
    return std::nullopt;
}

std::optional<bool> Pathfinder::flyGuard(PlayLayer* pl, uint8_t ch, PlayerSnap const& s, uint64_t tick,
                                         std::optional<bool> want) {
    // Flying planner: if what the corridor controller wants is predicted
    // to crash, find every moment within the next ~0.1 s where switching
    // survives and switch at a natural spot inside that window (or keep
    // going if not switching is what survives).
    (void)pl;
    if (!brainOn() || s.vx <= 0.3f) return want;
    if (m_pendingToggleAt[ch] >= 0 || m_tapReleaseAt[ch] >= 0) return want;
    if (!want && tick - m_lastGuardTick[ch] < 2) return want;
    m_lastGuardTick[ch] = tick;

    bool tapMode = s.mode == Mode::Ufo || s.mode == Mode::Swing;
    int fails = spotFailures(tick);
    int floorT = m_human.minStateTicks(fails);
    int tapLen = std::max(floorT, Humanizer::tapLength(m_human.params(), tick * 7 + ch));
    bool cur = m_hold[ch];
    SimState st = toSim(s, cur);
    int H = s.mode == Mode::Wave ? 40 : 52;
    SimWorld w = simWorld(ch, st, st.vx * static_cast<float>(H + 10) + 60.f);

    // Input pattern: switch at `at` (and back at `back`), -1 = never.
    auto pattern = [&](int at, int back) -> InputFn {
        if (tapMode) return [at, tapLen](int i) { return at >= 0 && i >= at && i < at + tapLen; };
        return [cur, at, back](int i) {
            bool v = cur;
            if (at >= 0 && i >= at) v = !v;
            if (back >= 0 && i >= back) v = !v;
            return v;
        };
    };
    auto run = [&](InputFn const& fn) { return Simulator::run(m_model, st, w, fn, H); };

    bool aToggles = tapMode ? (want.has_value() && *want) : (want.has_value() && *want != cur);
    auto ra = run(pattern(aToggles ? 0 : -1, -1));
    if (!ra.died) return want; // the controller's choice looks safe
    if (aToggles && !run(pattern(-1, -1)).died) return std::nullopt; // just don't switch yet

    // Earliest tick a switch is allowed (hold/release floor, 15 CPS cap).
    uint64_t sinceT = tick - m_lastToggle[ch];
    int earliest = sinceT >= static_cast<uint64_t>(floorT) ? 0 : static_cast<int>(floorT - sinceT);
    if (m_human.microAllowed(fails) && sinceT >= static_cast<uint64_t>(m_human.microTicks())) earliest = 0;
    int cd = m_human.pressCooldown();
    bool switchPresses = tapMode || !cur;
    if (switchPresses && cd > 0) {
        earliest = std::max<int>(earliest, static_cast<int>(std::max<int64_t>(0, m_lastPress[ch] + cd - static_cast<int64_t>(tick))));
    }

    std::vector<int> ok;
    int bestK = -1, bestTicks = ra.ticks;
    for (int k = earliest; k <= 24; k++) {
        auto r = run(pattern(k, -1));
        if (!r.died) ok.push_back(k);
        else if (r.ticks > bestTicks) { bestTicks = r.ticks; bestK = k; }
    }
    if (ok.empty() && !tapMode) {
        // Tight spot: switch and switch back.
        for (int k = earliest; k <= earliest + 8 && ok.empty(); k += 2) {
            for (int back : {k + floorT, k + floorT + 6, k + floorT + 12}) {
                if (!run(pattern(k, back)).died) { ok.push_back(k); break; }
            }
        }
    }
    if (ok.empty()) {
        if (bestK >= 0 && bestTicks > ra.ticks + 6) ok.push_back(bestK); // best effort
        else return want;
    }
    int lo = ok.front(), hi = lo;
    while (std::find(ok.begin(), ok.end(), hi + 1) != ok.end()) hi++;
    auto spot = Humanizer::humanSpot(m_human.params(), tick + static_cast<uint64_t>(lo), tick + static_cast<uint64_t>(hi),
                                     (tick + static_cast<uint64_t>(hi)) * 5 + ch);
    if (spot > tick + static_cast<uint64_t>(hi)) spot = tick + static_cast<uint64_t>(lo + (hi - lo) / 2);
    int k = static_cast<int>(spot - tick);

    bool to = tapMode ? true : !cur;
    if (k <= 0) {
        if (tapMode) m_tapReleaseAt[ch] = static_cast<int64_t>(tick) + tapLen;
        return to;
    }
    m_pendingToggleAt[ch] = static_cast<int64_t>(tick) + k;
    m_pendingToggleTo[ch] = to;
    if (tapMode) m_tapReleaseAt[ch] = static_cast<int64_t>(tick) + k + tapLen;
    return std::nullopt;
}

void Pathfinder::rankWithBrain(Segment& s, Cluster& c) {
    (void)s;
    if (!brainOn()) return;
    uint64_t ws = c.windowStart;
    auto t0 = c.trace.at(c.ch, ws);
    auto t1 = c.trace.at(c.ch, ws + 1);
    if (!t0 || !t1 || ws == 0) return;
    SimState st;
    st.x = t0->x;
    st.y = t0->y;
    st.vy = t0->vy;
    st.vx = t1->x - t0->x;
    st.w = t0->w;
    st.h = t0->h;
    st.grounded = t0->onGround;
    st.upside = t0->upsideDown;
    st.mini = t0->mini;
    st.mode = t0->mode;
    if (st.vx <= 0.3f) return;
    st.hold = stateFrom(c.attemptPlan, c.ch, ws - 1, false);

    uint64_t d = c.deathTick;
    int H = static_cast<int>(std::min<uint64_t>(d + 24 - ws, 360));
    SimWorld w = simWorld(c.ch, st, st.vx * static_cast<float>(H) + 60.f);

    auto holdsFor = [&](std::vector<Event> const& ev) {
        std::vector<uint8_t> holds(static_cast<size_t>(H), 0);
        bool h = st.hold;
        size_t j = 0;
        while (j < ev.size() && ev[j].tick < ws) { if (ev[j].ch == c.ch) h = ev[j].hold; j++; }
        for (int i = 0; i < H; i++) {
            uint64_t t = ws + static_cast<uint64_t>(i);
            while (j < ev.size() && ev[j].tick <= t) { if (ev[j].ch == c.ch) h = ev[j].hold; j++; }
            holds[static_cast<size_t>(i)] = h ? 1 : 0;
        }
        return holds;
    };
    auto simulate = [&](std::vector<Event> const& ev) {
        auto holds = holdsFor(ev);
        InputFn fn = [&holds](int i) { return holds[static_cast<size_t>(std::min<int>(i, static_cast<int>(holds.size()) - 1))] != 0; };
        return Simulator::run(m_model, st, w, fn, H);
    };

    // Only trust the brain here if it can reproduce the death that happened.
    auto replay = simulate(c.attemptPlan);
    uint64_t simDeath = ws + static_cast<uint64_t>(replay.ticks);
    bool agree = replay.died && absDiff(static_cast<int64_t>(simDeath), static_cast<int64_t>(d)) <= 12;
    m_model.noteAccuracy(agree);
    if (!agree) return;

    std::vector<Event> baseCh;
    for (auto const& e : c.base) if (e.ch == c.ch) baseCh.push_back(e);
    for (size_t i = c.next; i < c.queue.size(); i++) {
        auto& cand = c.queue[i];
        if (cand.live) continue;
        std::vector<Event> ev = baseCh;
        ev.insert(ev.end(), cand.events.begin(), cand.events.end());
        sortEvents(ev);
        auto r = simulate(ev);
        uint64_t at = ws + static_cast<uint64_t>(r.ticks);
        if (!r.died || at > d + 6) cand.score -= r.uncertain ? 2500.0 : 5000.0; // predicted to get past
        else if (at > d) cand.score -= static_cast<double>(std::min<uint64_t>(400, (at - d) * 20));
    }
    std::stable_sort(c.queue.begin() + static_cast<std::ptrdiff_t>(c.next), c.queue.end(),
                     [](Candidate const& a, Candidate const& b) { return a.score < b.score; });
}

// ---------------------------------------------------------------------------
// HUD
// ---------------------------------------------------------------------------

std::string Pathfinder::hudText(PlayLayer* pl) const {
    size_t cps = pl && pl->m_checkpointArray ? pl->m_checkpointArray->count() : 0;
    int segFails = m_segments.empty() ? 0 : m_segments.back().failures;
    std::string line = fmt::format("[Pathfinder: {} (F6) | Mode: {} (F7) | CPs: {} | Attempts: {}/{}]",
                                   m_enabled ? "ON" : "OFF", profileLabel(m_profile), cps, segFails, maxFailures());
    std::string status = m_status;
    if (m_fastForward) status += status.empty() ? ">> fast-forward" : "  >>";
    if (m_fastRespawnBroken) status += " (instant respawn off)";
    if (brainOn() && m_model.accuracySamples() > 0) {
        status += fmt::format("{}Brain {:.0f}% ({} orbs/pads learned)", status.empty() ? "" : " | ",
                              m_model.accuracy() * 100.f, m_model.learnedSpecials());
    }
    if (!status.empty()) line += "\n" + status;
    return line;
}

} // namespace pf
