#include "Pathfinder.hpp"

#include <algorithm>
#include <cmath>

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

int maxFailures() {
    return static_cast<int>(std::clamp<int64_t>(Mod::get()->getSettingValue<int64_t>("max-failures"), 10, 200));
}

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

TraceTick const* Trace::at(uint8_t ch, uint64_t tick) const {
    if (ch > 1 || tick < base) return nullptr;
    auto idx = static_cast<size_t>(tick - base);
    if (idx >= ticks[ch].size()) return nullptr;
    return &ticks[ch][idx];
}

Pathfinder& Pathfinder::get() {
    static Pathfinder inst;
    return inst;
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
        m_trace = {};
        m_trace.base = now;
        m_lastCheckpointTick = now;
        m_status = pl->m_isPracticeMode ? "Solving" : "Enter Practice Mode to start";
    }
}

void Pathfinder::onLevelInit(PlayLayer* pl) {
    m_inLevel = true;
    m_enabled = settingBool("enabled");
    m_segments.clear();
    m_index.clear();
    m_wide.clear();
    m_trace = {};
    m_totalDeaths = 0;
    m_hold[0] = m_hold[1] = false;
    m_lastTick = UINT64_MAX;
    for (int c = 0; c < 2; c++) {
        m_learnedFloor[c].clear();
        m_learnedCeil[c].clear();
        m_modeKnown[c] = false;
    }
    m_status = "Enter Practice Mode to start";
    (void)pl;
}

void Pathfinder::onLevelExit() {
    m_inLevel = false;
    m_segments.clear();
    m_index.clear();
    m_wide.clear();
    m_trace = {};
    m_hold[0] = m_hold[1] = false;
    m_pendingCheckpoint = false;
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
    s.human = Humanizer::makeParams(startTick * 31 + m_segments.size() * 7919 + 17);
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
    parent.lastTrace.base = m_trace.base;
    m_segments.push_back(makeSegment(tick, m_hold[0], m_hold[1], ours));
    m_trace = {};
    m_trace.base = tick;
    m_lastCheckpointTick = tick;
    m_lastSpecialPass = 0;
    m_cpInterval = 192 + static_cast<uint64_t>(Humanizer::hash01(tick * 13 + 5) * 96.f); // 0.8 - 1.2 s
    m_human.setParams(seg().human);
    m_human.clearHistory();
    m_status = "Checkpoint placed";
}

void Pathfinder::onBeforeReset(PlayLayer* pl) {
    if (!m_inLevel) return;
    releaseAll(pl);
}

void Pathfinder::onReset(PlayLayer* pl) {
    if (!m_inLevel) return;
    m_hold[0] = m_hold[1] = false;
    m_lastTick = UINT64_MAX;
    for (int c = 0; c < 2; c++) {
        m_pendingToggleAt[c] = -1;
        m_tapReleaseAt[c] = -1;
        m_havePrevY[c] = false;
        m_modeKnown[c] = false;
        m_lastToggle[c] = 0;
    }
    m_deathHandled = false;
    m_pendingCheckpoint = false;
    m_assistJumpedFor = 0;
    m_lastSpecialPass = 0;

    buildIndex(pl);
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
    m_cpInterval = 192 + static_cast<uint64_t>(Humanizer::hash01(now * 13 + s.attempts) * 96.f);
    m_human.setParams(s.human);
    m_human.reseed(now * 1000003ull + static_cast<uint64_t>(s.attempts) * 7 + m_segments.size());
    m_human.clearHistory();
    m_prevXs[0] = m_prevXs[1] = 0;

    if (!active(pl)) {
        m_status = m_enabled ? "Enter Practice Mode to start" : "Off (F6 to start)";
    } else if (!s.cluster) {
        m_status = "Exploring";
    }
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
        auto kind = classify(obj->m_objectType);
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

void Pathfinder::queryWindow(float x0, float x1, std::vector<WorldObject const*>& out) const {
    out.clear();
    float from = x0 - m_maxWidth - 1.f;
    auto it = std::lower_bound(m_index.begin(), m_index.end(), from,
                               [](WorldObject const& w, float v) { return w.minX < v; });
    for (; it != m_index.end() && it->minX <= x1; ++it) {
        if (it->maxX >= x0) out.push_back(&*it);
    }
    for (auto const& w : m_wide) {
        if (w.minX <= x1 && w.maxX >= x0) out.push_back(&w);
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
        if (isFlyMode(s.mode)) {
            float snapped = std::round(s.y / 30.f) * 30.f;
            m_modeFloor[ch] = std::max(90.f, snapped - 150.f);
            m_modeCeil[ch] = m_modeFloor[ch] + 300.f;
        }
    }
    if (!isFlyMode(s.mode)) return;
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
    if (isFlyMode(s.mode)) return flyController(pl, ch, s, tick);
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
    float py = s.y + s.vy * static_cast<float>(leadTicks);
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

    uint64_t since = tick - m_lastToggle[ch];
    if (since < static_cast<uint64_t>(floorT)) {
        // Hold/release floor. Only an about-to-crash prediction on a spot
        // that already failed >15 times may go down to a micro toggle.
        bool emergency = py < c.yMin - 2.f || py > c.yMax + 2.f;
        if (!(emergency && m_human.microAllowed(fails) && since >= static_cast<uint64_t>(m_human.microTicks()))) {
            return std::nullopt;
        }
    }

    bool safe = py > c.yMin + band * 0.5f && py < c.yMax - band * 0.5f;
    int jitter = m_human.toggleJitter(safe);
    if (*want && isTap) {
        int tapLen = floorT + static_cast<int>(Humanizer::hash01(tick * 7 + ch) * 4.f);
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
        // Default for ground modes is "nothing held": let go once the floor allows.
        if (tick - m_lastToggle[ch] >= static_cast<uint64_t>(floorT)) return false;
        return std::nullopt;
    }
    if (!settingBool("reactive-assist")) return std::nullopt;
    if (s.mode != Mode::Cube && s.mode != Mode::Robot) return std::nullopt;
    if (!s.onGround || s.vx <= 0.5f) return std::nullopt;
    if (tick - m_lastToggle[ch] < static_cast<uint64_t>(floorT)) return std::nullopt;

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
    // spike; aim for the 45-65% middle of that window, per-spike.
    float frac = 0.08f + 0.26f * (0.45f + 0.20f * Humanizer::hash01(key));
    if (best <= frac * span) {
        m_assistJumpedFor = key;
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
        if (!m_enabled) m_status = "Off (F6 to start)";
        else if (!pl->m_isPracticeMode) m_status = "Enter Practice Mode to start";
        else if (pl->m_isPlatformer) m_status = "Platformer levels aren't supported";
        return;
    }
    auto tick = static_cast<uint64_t>(pl->m_gameState.m_currentProgress);
    if (tick == m_lastTick) return;
    m_lastTick = tick;
    if (m_index.empty()) buildIndex(pl);
    if (m_segments.empty()) syncSegments(pl);

    // Checkpoints placed or removed by hand mid-attempt.
    size_t cps = pl->m_checkpointArray ? pl->m_checkpointArray->count() : 0;
    if (cps + 1 > m_segments.size()) onCheckpointAdded(pl, tick, m_expectOurs);
    while (m_segments.size() > cps + 1) m_segments.pop_back();

    if (!pl->m_player1 || pl->m_player1->m_isDead) return;

    int nch = channels(pl);
    PlayerSnap s0;
    for (uint8_t ch = 0; ch < nch; ch++) {
        PlayerObject* p = ch == 0 ? pl->m_player1 : pl->m_player2;
        PlayerSnap s = snap(p, ch);
        if (!s.valid) continue;
        updateBounds(ch, s);
        bool orb = touchingOrb(s);

        auto& sg = seg();
        if (tick < sg.lockedUntil) {
            press(pl, ch, planStateAt(sg, ch, tick), tick, false);
        } else if (auto want = livePolicy(pl, ch, s, tick)) {
            press(pl, ch, *want, tick, true);
        }

        auto& tr = m_trace.ticks[ch];
        if (tick >= m_trace.base) {
            auto idx = static_cast<size_t>(tick - m_trace.base);
            TraceTick tt{s.x, s.y, s.vy, s.onGround, m_hold[ch], orb, s.mode};
            if (idx > 200000) {
                // absurdly long segment without a checkpoint; stop tracing
            } else {
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

    auto const& sg = seg();
    if (sg.cluster && tick > sg.cluster->deathTick + 6 && tick < sg.cluster->deathTick + 10) {
        m_status = fmt::format("Cleared the {:.0f}% spot", pl->getCurrentPercent());
    }
}

bool Pathfinder::safeForCheckpoint(PlayLayer* pl, PlayerSnap const& s, uint64_t tick) const {
    auto const& sg = m_segments.back();
    if (sg.cluster && tick <= sg.cluster->deathTick + 12) return false; // not past the known trouble yet
    if (tick < sg.lockedUntil) return false;
    if (tick < sg.startTick + 60) return false;
    if (m_pendingToggleAt[0] >= 0 || m_tapReleaseAt[0] >= 0) return false;

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
    if (pl->m_gameState.m_isDualMode && pl->m_player2) {
        uint8_t ch2 = channels(pl) == 2 ? 1 : 0;
        PlayerSnap s2 = snap(pl->m_player2, ch2);
        if (ch2 == 0) { s2.vy = 0; } // no separate history for p2 in single-channel dual
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
    size_t before = pl->m_checkpointArray->count();
    m_expectOurs = true;
    pl->markCheckpoint();
    size_t after = pl->m_checkpointArray->count();
    if (after > before && m_segments.size() < after + 1) {
        onCheckpointAdded(pl, static_cast<uint64_t>(pl->m_gameState.m_currentProgress), true);
    }
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

void Pathfinder::rebuildBase(Segment& s, Cluster& c) {
    c.base.clear();
    for (auto const& e : c.attemptPlan) {
        if (e.tick < c.windowStart || e.ch != c.ch) c.base.push_back(e);
    }
    (void)s;
}

void Pathfinder::openCluster(Segment& s, uint64_t deathTick, uint8_t ch, Mode mode, int side, bool doomed,
                             Trace const& trace, std::vector<Event> const& attemptPlan) {
    Cluster c;
    c.deathTick = deathTick;
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
    bool holdAtWs = stateFrom(c.base, c.ch, ws, false);
    std::vector<Candidate> out;
    std::unordered_set<uint64_t> seen;

    auto add = [&](Candidate cand) {
        sortEvents(cand.events);
        cand.key = candidateKey(c.base, cand);
        if (s.tried.count(cand.key) || seen.count(cand.key)) return;
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

    // Flying: first try steering the humanized controller differently.
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
                auto order = Humanizer::centeredOrder(runStart, t - 1);
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
                auto order = Humanizer::centeredOrder(lo, t);
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
        for (uint64_t t = ws; t + 1 < d; t += 2) ticks.push_back(t);
        std::stable_sort(ticks.begin(), ticks.end(), [&](uint64_t a, uint64_t b) {
            auto da = a > ideal ? a - ideal : ideal - a;
            auto db = b > ideal ? b - ideal : ideal - b;
            return da < db;
        });
        for (size_t i = 0; i < ticks.size(); i++) anchors.push_back({ticks[i], groupBase + 40 + i * 1.0});
    }

    std::vector<int> lengths;
    switch (c.mode) {
        case Mode::Cube: lengths = {floorT, floorT + 12, 44}; break;
        case Mode::Robot: lengths = {floorT, 24, 34, 48}; break;
        case Mode::Ship:
        case Mode::Wave: lengths = {floorT, 26, 44}; break;
        default: lengths = {floorT}; break;
    }

    std::unordered_set<uint64_t> usedTick;
    for (auto const& a : anchors) {
        if (holdAtWs && a.tick < ws + static_cast<uint64_t>(floorT)) continue;
        if (!usedTick.insert(a.tick).second) continue;
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

    std::stable_sort(out.begin(), out.end(), [](Candidate const& a, Candidate const& b) { return a.score < b.score; });

    // Anti-robot filter: push candidates whose resulting click rhythm is
    // machine-regular behind otherwise-equal ones.
    for (auto& cand : out) {
        std::vector<uint64_t> ticks;
        for (auto const& e : c.base) if (e.ch == c.ch && e.tick + 400 >= ws) ticks.push_back(e.tick);
        for (auto const& e : cand.events) ticks.push_back(e.tick);
        std::sort(ticks.begin(), ticks.end());
        cand.score += Humanizer::periodicity(ticks) * 25.0;
    }
    std::stable_sort(out.begin(), out.end(), [](Candidate const& a, Candidate const& b) { return a.score < b.score; });

    if (out.size() > 400) out.resize(400);
    c.queue = std::move(out);
    c.next = 0;
}

void Pathfinder::addPairs(Segment& s, Cluster& c) {
    c.pairsAdded = true;
    int floorT = s.human.baseFloor;
    std::vector<Candidate> singles;
    for (auto const& cand : c.queue) {
        if (cand.live) continue;
        int presses = 0;
        for (auto const& e : cand.events) presses += e.hold ? 1 : 0;
        if (presses == 1) singles.push_back(cand);
        if (singles.size() >= 10) break;
    }
    std::vector<Candidate> pairs;
    double sc = 500;
    for (auto const& first : singles) {
        uint64_t rel = 0;
        for (auto const& e : first.events) if (!e.hold) rel = std::max(rel, e.tick);
        for (int gap : {floorT, floorT + 10, 28, 44}) {
            uint64_t t2 = rel + static_cast<uint64_t>(gap);
            if (t2 > c.deathTick + 10) continue;
            Candidate p = first;
            p.events.push_back({t2, true, c.ch});
            p.events.push_back({t2 + static_cast<uint64_t>(floorT), false, c.ch});
            sortEvents(p.events);
            p.key = candidateKey(c.base, p);
            p.score = sc++;
            if (!s.tried.count(p.key)) pairs.push_back(std::move(p));
        }
    }
    c.queue.insert(c.queue.end(), pairs.begin(), pairs.end());
}

void Pathfinder::addMicro(Segment& s, Cluster& c) {
    c.microAdded = true;
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
    uint64_t ideal = d > 20 ? d - 20 : ws;
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
            if (!s.tried.count(cand.key)) extra.push_back(std::move(cand));
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

bool Pathfinder::applyNextCandidate(Segment& s) {
    if (!s.cluster) return false;
    auto& c = *s.cluster;
    if (c.tries > 15 && !c.microAdded) addMicro(s, c);

    for (int guard = 0; guard < 8; guard++) {
        while (c.next < c.queue.size()) {
            Candidate cand = c.queue[c.next++];
            if (s.tried.count(cand.key)) continue;
            s.tried.insert(cand.key);

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
            m_status = fmt::format("{} @ tick {}: try {} ({})", c.doomed ? "Re-routing" : "Fixing", c.deathTick,
                                   c.tries + 1, cand.live ? "steer" : (cand.events.empty() ? "no input" : "clicks"));
            return true;
        }
        if (!c.pairsAdded) { addPairs(s, c); continue; }
        if (!c.microAdded) { addMicro(s, c); continue; }
        if (c.widen < 2) {
            c.widen++;
            uint64_t L = lookback(c.mode) / 2;
            c.windowStart = std::max(s.startTick, c.windowStart > L ? c.windowStart - L : 0);
            rebuildBase(s, c);
            c.pairsAdded = c.microAdded = false;
            buildQueue(s, c);
            continue;
        }
        break;
    }
    return false;
}

void Pathfinder::backtrack(PlayLayer* pl) {
    for (int depth = 0; depth < 4 && m_segments.size() > 1; depth++) {
        uint64_t childStart = m_segments.back().startTick;
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
        Mode mode = Mode::Cube;
        if (auto tt = parent.lastTrace.at(0, childStart > 0 ? childStart - 1 : 0)) mode = tt->mode;
        openCluster(parent, childStart, 0, mode, 0, true, parent.lastTrace, parent.plan);
        m_status = fmt::format("Backtracked: checkpoint {} was a dead end", m_segments.size());
        if (parent.failures <= maxFailures() && applyNextCandidate(parent)) return;
    }
    // Back at the first segment with nothing left: start the search over with a clean slate.
    auto& root = seg();
    root.failures = 0;
    root.tried.clear();
    if (root.cluster) {
        auto c = *root.cluster;
        openCluster(root, c.deathTick, c.ch, c.mode, c.side, c.doomed, c.trace, c.attemptPlan);
        applyNextCandidate(root);
    }
}

void Pathfinder::onDeath(PlayLayer* pl, PlayerObject* player, GameObject* killer) {
    if (!m_inLevel || !active(pl) || m_deathHandled) return;
    m_deathHandled = true;
    m_totalDeaths++;
    if (m_segments.empty()) syncSegments(pl);

    auto tick = static_cast<uint64_t>(pl->m_gameState.m_currentProgress);
    uint8_t ch = (player == pl->m_player2 && channels(pl) == 2) ? 1 : 0;
    PlayerSnap ps = snap(player, ch);
    releaseAll(pl);
    m_pendingCheckpoint = false;

    auto& s = seg();
    s.failures++;

    int side = 0;
    if (killer && killer != pl->m_anticheatSpike) {
        side = killer->getObjectRect().getMidY() > ps.y ? 1 : -1;
    } else if (isFlyMode(ps.mode) && ch <= 1) {
        side = (m_modeCeil[ch] - ps.y) < (ps.y - m_modeFloor[ch]) ? 1 : -1;
    }
    Mode mode = ps.mode;
    if (auto tt = m_trace.at(ch, tick > 3 ? tick - 3 : 0)) mode = tt->mode;

    if (s.failures > maxFailures()) {
        if (m_segments.size() > 1) {
            backtrack(pl);
            return;
        }
        s.failures = 0; // the very first segment can't be abandoned
    }

    if (s.cluster) {
        auto& c = *s.cluster;
        bool sameSpot = tick + 6 >= c.deathTick && tick <= c.deathTick + 6;
        bool worseInWindow = tick < c.deathTick && tick >= c.windowStart;
        if (sameSpot || worseInWindow) {
            c.tries++;
            if (!applyNextCandidate(s)) {
                if (m_segments.size() > 1) backtrack(pl);
                else {
                    s.tried.clear();
                    openCluster(s, c.deathTick, c.ch, c.mode, c.side, false, c.trace, c.attemptPlan);
                    applyNextCandidate(s);
                }
            }
            return;
        }
    }
    openCluster(s, tick, ch, mode, side, false, m_trace, s.plan);
    if (!applyNextCandidate(s) && m_segments.size() > 1) backtrack(pl);
}

// ---------------------------------------------------------------------------
// HUD
// ---------------------------------------------------------------------------

std::string Pathfinder::hudText(PlayLayer* pl) const {
    size_t cps = pl && pl->m_checkpointArray ? pl->m_checkpointArray->count() : 0;
    int segFails = m_segments.empty() ? 0 : m_segments.back().failures;
    std::string line = fmt::format("[Pathfinder: {} | CPs: {} | Seg Attempts: {}/{} | Mode: Humanized]",
                                   m_enabled ? "ON" : "OFF", cps, segFails, maxFailures());
    if (!m_status.empty()) line += "\n" + m_status;
    return line;
}

} // namespace pf
