#include "Brain.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace pf {

void PhysicsModel::Stat::add(float v, float alpha) {
    // Priors count as a few samples, so the first real samples move the
    // value quickly but one outlier can't wreck it.
    float a = std::max(alpha, 1.f / static_cast<float>(n + 4));
    value += (v - value) * a;
    n = std::min(n + 1, 1000000);
}

PhysicsModel::PhysicsModel() {
    // Rough priors (gravity-relative, world units per tick at 240 TPS).
    auto setAccel = [&](Mode m, float noHold, float holdAsc, float holdDesc) {
        for (int mini = 0; mini < 2; mini++) {
            m_accel[mi(m)][mini][0][0].value = noHold;
            m_accel[mi(m)][mini][0][1].value = noHold;
            m_accel[mi(m)][mini][1][1].value = holdAsc;
            m_accel[mi(m)][mini][1][0].value = holdDesc;
        }
    };
    setAccel(Mode::Cube, -0.073f, -0.073f, -0.073f);
    setAccel(Mode::Robot, -0.073f, -0.02f, -0.073f);
    setAccel(Mode::Ball, -0.06f, -0.06f, -0.06f);
    setAccel(Mode::Spider, -0.07f, -0.07f, -0.07f);
    setAccel(Mode::Ufo, -0.045f, -0.045f, -0.045f);
    setAccel(Mode::Swing, -0.04f, -0.04f, -0.04f);
    setAccel(Mode::Ship, -0.03f, 0.035f, 0.035f);
    setAccel(Mode::Wave, 0.f, 0.f, 0.f);

    auto setImp = [&](Mode m, float normal, float mini) {
        m_impulse[mi(m)][0].value = normal;
        m_impulse[mi(m)][1].value = mini;
    };
    setImp(Mode::Cube, 3.0f, 2.4f);
    setImp(Mode::Robot, 2.3f, 1.9f);
    setImp(Mode::Ufo, 2.0f, 1.7f);

    auto setV = [&](Mode m, float up, float down) {
        for (int mini = 0; mini < 2; mini++) {
            m_vUp[mi(m)][mini].value = up;
            m_vDown[mi(m)][mini].value = down;
        }
    };
    setV(Mode::Cube, 3.4f, -4.0f);
    setV(Mode::Robot, 3.2f, -4.0f);
    setV(Mode::Ball, 4.0f, -4.0f);
    setV(Mode::Spider, 4.5f, -4.5f);
    setV(Mode::Ufo, 3.0f, -2.8f);
    setV(Mode::Swing, 2.5f, -2.5f);
    setV(Mode::Ship, 2.2f, -2.6f);
    setV(Mode::Wave, 4.0f, -4.0f);

    m_wave[0].value = 1.0f;
    m_wave[1].value = 2.0f;
}

void PhysicsModel::observe(Mode mode, bool mini, bool upside, bool holdApplied, bool groundedBefore,
                           bool groundedAfter, float vyBefore, float vyAfter, float vx, int ticksSincePress) {
    float g = upside ? -1.f : 1.f;
    float vb = vyBefore * g;
    float va = vyAfter * g;
    float dv = va - vb;
    int mn = mini ? 1 : 0;
    int m = mi(mode);
    bool recentPress = ticksSincePress >= 0 && ticksSincePress <= 2;

    // Jump / UFO tap strength: the sudden upward speed right after a press.
    if (recentPress && dv > 0.5f) {
        if ((mode == Mode::Cube || mode == Mode::Robot) && groundedBefore && !groundedAfter) {
            m_impulse[m][mn].add(va, 0.15f);
            m_samples++;
        } else if (mode == Mode::Ufo) {
            m_impulse[m][mn].add(va, 0.15f);
            m_samples++;
        }
        return;
    }
    if (groundedBefore || groundedAfter) return;

    if (mode == Mode::Wave) {
        if (vx > 0.5f && !recentPress && std::abs(va) > 0.2f) {
            m_wave[mn].add(std::abs(va) / vx, 0.1f);
            m_samples++;
        }
        return;
    }
    if (recentPress) return;
    if (std::abs(dv) < 1.0f) {
        m_accel[m][mn][holdApplied ? 1 : 0][vb > 0.f ? 1 : 0].add(dv, 0.08f);
        m_samples++;
    }
    // Terminal speed: moving fast and no longer accelerating.
    if (std::abs(dv) < 0.01f && std::abs(va) > 1.f) {
        if (va > 0.f) m_vUp[m][mn].add(va, 0.2f);
        else m_vDown[m][mn].add(va, 0.2f);
    }
}

float PhysicsModel::accel(Mode m, bool mini, bool hold, bool ascending) const {
    return m_accel[mi(m)][mini ? 1 : 0][hold ? 1 : 0][ascending ? 1 : 0].value;
}

float PhysicsModel::impulse(Mode m, bool mini) const { return m_impulse[mi(m)][mini ? 1 : 0].value; }
float PhysicsModel::vmaxUp(Mode m, bool mini) const { return std::max(0.5f, m_vUp[mi(m)][mini ? 1 : 0].value); }
float PhysicsModel::vmaxDown(Mode m, bool mini) const { return std::min(-0.5f, m_vDown[mi(m)][mini ? 1 : 0].value); }
float PhysicsModel::waveSlope(bool mini) const { return std::max(0.2f, m_wave[mini ? 1 : 0].value); }

bool PhysicsModel::flipsGravity(int type) {
    return type == static_cast<int>(GameObjectType::GravityRing) || type == static_cast<int>(GameObjectType::GreenRing) ||
           type == static_cast<int>(GameObjectType::GravityPad);
}

bool PhysicsModel::unsupported(int type) {
    return type == static_cast<int>(GameObjectType::CustomRing) || type == static_cast<int>(GameObjectType::DashRing) ||
           type == static_cast<int>(GameObjectType::GravityDashRing) || type == static_cast<int>(GameObjectType::SpiderOrb) ||
           type == static_cast<int>(GameObjectType::SpiderPad) || type == static_cast<int>(GameObjectType::TeleportOrb);
}

SpecialEffect PhysicsModel::special(int type, Mode m, bool mini) const {
    SpecialEffect e;
    if (type < 0 || type >= 48 || unsupported(type)) return e;
    e.flip = flipsGravity(type);
    auto const& st = m_special[type][mi(m)][mini ? 1 : 0];
    if (st.n > 0) {
        e.known = true;
        e.vyRel = st.value;
        return e;
    }
    // Never seen it used in this mode yet: educated guess relative to this
    // mode's jump (or the cube's), flagged so a predicted death after it
    // isn't trusted.
    float imp = impulse(m, mini);
    if (imp <= 0.5f) imp = impulse(Mode::Cube, mini);
    using T = GameObjectType;
    float f = 0.f;
    switch (static_cast<T>(type)) {
        case T::YellowJumpRing: f = 1.0f; break;
        case T::PinkJumpRing: f = 0.72f; break;
        case T::RedJumpRing: f = 1.38f; break;
        case T::GravityRing: f = 0.4f; break;
        case T::GreenRing: f = 1.0f; break;
        case T::DropRing: f = -1.5f; break;
        case T::YellowJumpPad: f = 1.45f; break;
        case T::PinkJumpPad: f = 0.95f; break;
        case T::RedJumpPad: f = 1.9f; break;
        case T::GravityPad: f = 0.6f; break;
        default: return e;
    }
    e.known = true;
    e.prior = true;
    e.vyRel = imp * f;
    return e;
}

void PhysicsModel::noteMiss(Mode m) {
    m_misses[mi(m)]++;
    m_margin[mi(m)] = std::min(5.f, m_margin[mi(m)] + 0.75f);
}

void PhysicsModel::noteHit(Mode m) { m_hits[mi(m)]++; }

bool PhysicsModel::trusted(Mode m) const {
    int h = m_hits[mi(m)], x = m_misses[mi(m)];
    if (x < 6) return true;
    return static_cast<float>(h + 2) / static_cast<float>(h + x + 2) >= 0.4f;
}

void PhysicsModel::observeSpecial(int type, Mode m, bool mini, float vyRelAfter) {
    if (type < 0 || type >= 48 || unsupported(type)) return;
    auto& st = m_special[type][mi(m)][mini ? 1 : 0];
    if (st.n == 0) {
        st.value = vyRelAfter;
        st.n = 1;
    } else {
        st.add(vyRelAfter, 0.25f);
    }
    m_samples++;
}

int PhysicsModel::learnedSpecials() const {
    int n = 0;
    for (auto const& a : m_special) for (auto const& b : a) for (auto const& c : b) n += c.n > 0 ? 1 : 0;
    return n;
}

void PhysicsModel::noteAccuracy(bool ok) {
    m_accN++;
    float a = std::max(0.05f, 1.f / static_cast<float>(m_accN));
    m_accuracy += ((ok ? 1.f : 0.f) - m_accuracy) * a;
}

std::string PhysicsModel::serialize() const {
    std::ostringstream o;
    o << "v2";
    auto put = [&](Stat const& s) { o << ' ' << s.value << ' ' << std::min(s.n, 50); };
    for (auto& a : m_accel) for (auto& b : a) for (auto& c : b) for (auto& d : c) put(d);
    for (auto& a : m_impulse) for (auto& b : a) put(b);
    for (auto& a : m_vUp) for (auto& b : a) put(b);
    for (auto& a : m_vDown) for (auto& b : a) put(b);
    for (auto& a : m_wave) put(a);
    for (auto& a : m_special) for (auto& b : a) for (auto& c : b) put(c);
    return o.str();
}

void PhysicsModel::deserialize(std::string const& str) {
    std::istringstream in(str);
    std::string tag;
    if (!(in >> tag) || (tag != "v1" && tag != "v2")) return;
    PhysicsModel tmp;
    auto get = [&](Stat& s) {
        float v = 0;
        int n = 0;
        if (!(in >> v >> n)) return false;
        if (!std::isfinite(v)) return false;
        s.value = v;
        s.n = n;
        return true;
    };
    for (auto& a : tmp.m_accel) for (auto& b : a) for (auto& c : b) for (auto& d : c) if (!get(d)) return;
    for (auto& a : tmp.m_impulse) for (auto& b : a) if (!get(b)) return;
    for (auto& a : tmp.m_vUp) for (auto& b : a) if (!get(b)) return;
    for (auto& a : tmp.m_vDown) for (auto& b : a) if (!get(b)) return;
    for (auto& a : tmp.m_wave) if (!get(a)) return;
    if (tag == "v2") {
        for (auto& a : tmp.m_special) for (auto& b : a) for (auto& c : b) if (!get(c)) return;
        std::copy(&tmp.m_special[0][0][0], &tmp.m_special[0][0][0] + 48 * 8 * 2, &m_special[0][0][0]);
    }
    // Only take it if the whole record parsed.
    std::copy(&tmp.m_accel[0][0][0][0], &tmp.m_accel[0][0][0][0] + 8 * 2 * 2 * 2, &m_accel[0][0][0][0]);
    std::copy(&tmp.m_impulse[0][0], &tmp.m_impulse[0][0] + 8 * 2, &m_impulse[0][0]);
    std::copy(&tmp.m_vUp[0][0], &tmp.m_vUp[0][0] + 8 * 2, &m_vUp[0][0]);
    std::copy(&tmp.m_vDown[0][0], &tmp.m_vDown[0][0] + 8 * 2, &m_vDown[0][0]);
    std::copy(&tmp.m_wave[0], &tmp.m_wave[0] + 2, &m_wave[0]);
}

// ---------------------------------------------------------------------------
// simulator
// ---------------------------------------------------------------------------

SimResult Simulator::run(PhysicsModel const& m, SimState s, SimWorld const& w, InputFn const& input, int horizon) {
    SimResult res;
    float g = s.upside ? -1.f : 1.f;
    bool prevHold = s.hold;
    // Modes that land on / slide along both the floor and the ceiling side.
    bool both = s.mode != Mode::Cube && s.mode != Mode::Robot;
    float maxW = 0;
    for (auto const& o : w.objs) maxW = std::max(maxW, o.r.size.width);
    size_t start = 0;
    std::vector<uint8_t> used(w.objs.size(), 0);
    bool afterModeChange = false; // also set after a guessed orb/pad effect
    float margin = m.hazardMargin(s.mode);
    auto finishDeath = [&]() {
        // After a mode change the learned numbers for the new mode may be
        // thin: don't let the bot act on such a prediction.
        if (afterModeChange) res.uncertain = true;
        else res.died = true;
    };
    // Known speed portal speeds (units per second): 0.5x, 1x, 2x, 3x, 4x.
    static constexpr float SPEEDS[5] = {251.16f, 311.58f, 387.42f, 468.0f, 576.0f};
    auto speedScale = [&](float vx) {
        // Calibrate table speeds to how this game measures them per tick.
        float perSec = vx * 240.f;
        float best = SPEEDS[1];
        for (float sp : SPEEDS) if (std::abs(sp - perSec) < std::abs(best - perSec)) best = sp;
        return vx / (best / 240.f);
    };
    float scale = s.vx > 0.f ? speedScale(s.vx) : 1.f;

    auto supportAt = [&](float x0, float x1, float edgeY, bool below, size_t from) {
        // Is there a surface touching edgeY (within 2 units) under/over [x0, x1]?
        for (size_t i = from; i < w.objs.size(); i++) {
            auto const& o = w.objs[i];
            if (o.r.getMinX() > x1) break;
            if (o.kind != ObjKind::Solid || o.r.getMaxX() < x0) continue;
            float surf = below ? o.r.getMaxY() : o.r.getMinY();
            if (std::abs(surf - edgeY) <= 2.f) return true;
        }
        return false;
    };

    for (int i = 0; i < horizon; i++) {
        res.ticks = i + 1;
        bool hold = input(i);
        bool pressEdge = hold && !prevHold;

        while (start < w.objs.size() && w.objs[start].r.getMinX() + maxW < s.x - s.w) start++;

        // Walked off an edge?
        if (s.grounded) {
            float half = s.h * 0.5f;
            bool below = g > 0.f;
            float edge = below ? s.y - half : s.y + half;
            bool onBound = below ? std::abs(edge - w.floorY) <= 2.f : std::abs(edge - w.ceilY) <= 2.f;
            if (!onBound && !supportAt(s.x - s.w * 0.5f, s.x + s.w * 0.5f, edge, below, start)) s.grounded = false;
        }

        float vyRel = s.vy * g;
        switch (s.mode) {
            case Mode::Cube:
            case Mode::Robot:
                if (hold && s.grounded) { // holding through a landing jumps again
                    vyRel = m.impulse(s.mode, s.mini);
                    s.grounded = false;
                }
                break;
            case Mode::Ball:
                if (pressEdge && s.grounded) {
                    s.upside = !s.upside;
                    g = -g;
                    vyRel = 0;
                    s.grounded = false;
                }
                break;
            case Mode::Spider:
                if (pressEdge && s.grounded) {
                    // Teleport to the nearest surface on the other side.
                    float half = s.h * 0.5f;
                    float best = g > 0.f ? w.ceilY : w.floorY;
                    for (size_t k = start; k < w.objs.size(); k++) {
                        auto const& o = w.objs[k];
                        if (o.r.getMinX() > s.x + s.w * 0.5f) break;
                        if (o.kind != ObjKind::Solid || o.r.getMaxX() < s.x - s.w * 0.5f) continue;
                        if (g > 0.f && o.r.getMinY() >= s.y + half - 1.f) best = std::min(best, o.r.getMinY());
                        if (g < 0.f && o.r.getMaxY() <= s.y - half + 1.f) best = std::max(best, o.r.getMaxY());
                    }
                    if (best >= 1e8f || best <= -1e8f) {
                        res.uncertain = true;
                        return res;
                    }
                    s.y = g > 0.f ? best - half : best + half;
                    s.upside = !s.upside;
                    g = -g;
                    vyRel = 0;
                    s.grounded = true;
                }
                break;
            case Mode::Ufo:
                if (pressEdge) {
                    vyRel = m.impulse(Mode::Ufo, s.mini);
                    s.grounded = false;
                }
                break;
            case Mode::Swing:
                if (pressEdge) {
                    s.upside = !s.upside;
                    g = -g;
                    vyRel = s.vy * g; // same world speed, new gravity frame
                    s.grounded = false;
                }
                break;
            default:
                break;
        }

        if (s.mode == Mode::Ship) {
            vyRel += m.accel(Mode::Ship, s.mini, hold, vyRel > 0.f);
            vyRel = std::clamp(vyRel, m.vmaxDown(Mode::Ship, s.mini), m.vmaxUp(Mode::Ship, s.mini));
            if (s.grounded && vyRel > 0.f) s.grounded = false;
        } else if (s.mode == Mode::Wave) {
            vyRel = (hold ? 1.f : -1.f) * m.waveSlope(s.mini) * s.vx;
            s.grounded = false;
        } else if (!s.grounded) {
            vyRel += m.accel(s.mode, s.mini, hold, vyRel > 0.f);
            vyRel = std::clamp(vyRel, m.vmaxDown(s.mode, s.mini), m.vmaxUp(s.mode, s.mini));
        } else {
            vyRel = 0.f;
        }
        s.vy = vyRel * g;

        float prevY = s.y;
        s.x += s.vx;
        s.y += s.vy;
        float half = s.h * 0.5f;
        cocos2d::CCRect pr(s.x - s.w * 0.5f, s.y - half, s.w, s.h);

        for (size_t k = start; k < w.objs.size(); k++) {
            auto const& o = w.objs[k];
            if (o.r.getMinX() > pr.getMaxX()) break;
            if (o.kind == ObjKind::Hazard) {
                cocos2d::CCRect hr(o.r.getMinX() - margin, o.r.getMinY() - margin, o.r.size.width + 2 * margin,
                                   o.r.size.height + 2 * margin);
                if (hr.intersectsRect(pr)) {
                    finishDeath();
                    return res;
                }
                continue;
            }
            if (!o.r.intersectsRect(pr)) continue;
            if (o.kind == ObjKind::Speed) {
                if (!used[k] && o.speed > 0.f) {
                    used[k] = 1;
                    s.vx = o.speed * scale;
                }
                continue;
            }
            if (o.kind == ObjKind::Orb || o.kind == ObjKind::Pad) {
                // Orbs fire while the button is held (a click or a held
                // button touching it); pads fire on touch.
                if (used[k] || (o.kind == ObjKind::Orb && !hold)) continue;
                auto eff = m.special(o.type, s.mode, s.mini);
                if (!eff.known) {
                    res.uncertain = true;
                    return res;
                }
                used[k] = 1;
                if (eff.prior) afterModeChange = true;
                if (eff.flip) {
                    s.upside = !s.upside;
                    g = -g;
                }
                s.vy = eff.vyRel * g;
                s.grounded = false;
                continue;
            }
            if (o.kind == ObjKind::Portal) {
                if (used[k]) continue;
                used[k] = 1;
                using T = GameObjectType;
                auto t = static_cast<T>(o.type);
                auto setMode = [&](Mode nm) {
                    if (s.mode != nm) {
                        s.mode = nm;
                        s.vy *= 0.5f;
                        afterModeChange = true;
                        both = s.mode != Mode::Cube && s.mode != Mode::Robot;
                        margin = m.hazardMargin(s.mode);
                    }
                };
                switch (t) {
                    case T::InverseGravityPortal:
                        if (!s.upside) { s.upside = true; g = -1.f; s.grounded = false; }
                        break;
                    case T::NormalGravityPortal:
                        if (s.upside) { s.upside = false; g = 1.f; s.grounded = false; }
                        break;
                    case T::GravityTogglePortal:
                        s.upside = !s.upside;
                        g = -g;
                        s.grounded = false;
                        break;
                    case T::MiniSizePortal:
                        if (!s.mini) { s.mini = true; s.w *= 0.6f; s.h *= 0.6f; half = s.h * 0.5f; }
                        break;
                    case T::RegularSizePortal:
                        if (s.mini) { s.mini = false; s.w /= 0.6f; s.h /= 0.6f; half = s.h * 0.5f; }
                        break;
                    case T::ShipPortal: setMode(Mode::Ship); break;
                    case T::CubePortal: setMode(Mode::Cube); break;
                    case T::BallPortal: setMode(Mode::Ball); break;
                    case T::UfoPortal: setMode(Mode::Ufo); break;
                    case T::WavePortal: setMode(Mode::Wave); break;
                    case T::RobotPortal: setMode(Mode::Robot); break;
                    case T::SpiderPortal: setMode(Mode::Spider); break;
                    case T::SwingPortal: setMode(Mode::Swing); break;
                    case T::InverseMirrorPortal:
                    case T::NormalMirrorPortal:
                        break; // visual only
                    default:
                        res.uncertain = true; // dual / solo / teleport portals
                        return res;
                }
                continue;
            }
            if (o.kind != ObjKind::Solid || o.slope) {
                res.uncertain = true; // slopes aren't modelled
                return res;
            }
            float prevBottom = prevY - half;
            float prevTop = prevY + half;
            float tol = 2.f + std::abs(s.vy);
            bool fromAbove = prevBottom >= o.r.getMaxY() - tol && s.vy <= 0.f;
            bool fromBelow = prevTop <= o.r.getMinY() + tol && s.vy >= 0.f;
            if (fromAbove) {
                bool floorHit = g > 0.f;
                if (!floorHit && !both) {
                    finishDeath();
                    return res;
                }
                s.y = o.r.getMaxY() + half;
                s.vy = 0.f;
                s.grounded = floorHit;
            } else if (fromBelow) {
                bool floorHit = g < 0.f;
                if (!floorHit && !both) {
                    finishDeath();
                    return res;
                }
                s.y = o.r.getMinY() - half;
                s.vy = 0.f;
                s.grounded = floorHit;
            } else {
                // Blocks only kill through the player's small inner hitbox;
                // grazing an edge with the outer box is survivable.
                cocos2d::CCRect inner(s.x - s.w * 0.15f, s.y - s.h * 0.15f, s.w * 0.3f, s.h * 0.3f);
                if (o.r.intersectsRect(inner)) {
                    finishDeath(); // ran into the side of a block
                    return res;
                }
                continue;
            }
            pr = cocos2d::CCRect(s.x - s.w * 0.5f, s.y - half, s.w, s.h);
        }

        if (s.y - half < w.floorY) {
            s.y = w.floorY + half;
            s.vy = 0.f;
            s.grounded = g > 0.f;
        }
        if (s.y + half > w.ceilY) {
            if (!both && g > 0.f) {
                finishDeath();
                return res;
            }
            s.y = w.ceilY - half;
            s.vy = 0.f;
            s.grounded = g < 0.f;
        }
        prevHold = hold;
    }
    return res;
}

} // namespace pf
