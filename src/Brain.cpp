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

void PhysicsModel::noteAccuracy(bool ok) {
    m_accN++;
    float a = std::max(0.05f, 1.f / static_cast<float>(m_accN));
    m_accuracy += ((ok ? 1.f : 0.f) - m_accuracy) * a;
}

std::string PhysicsModel::serialize() const {
    std::ostringstream o;
    o << "v1";
    auto put = [&](Stat const& s) { o << ' ' << s.value << ' ' << std::min(s.n, 50); };
    for (auto& a : m_accel) for (auto& b : a) for (auto& c : b) for (auto& d : c) put(d);
    for (auto& a : m_impulse) for (auto& b : a) put(b);
    for (auto& a : m_vUp) for (auto& b : a) put(b);
    for (auto& a : m_vDown) for (auto& b : a) put(b);
    for (auto& a : m_wave) put(a);
    return o.str();
}

void PhysicsModel::deserialize(std::string const& str) {
    std::istringstream in(str);
    std::string tag;
    if (!(in >> tag) || tag != "v1") return;
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
            if (!o.r.intersectsRect(pr)) continue;
            if (o.kind == ObjKind::Hazard) {
                res.died = true;
                return res;
            }
            if (o.kind == ObjKind::Orb && !hold) continue; // passing an orb without clicking does nothing
            if (o.kind != ObjKind::Solid || o.slope) {
                res.uncertain = true; // orb click / pad / portal / slope: not modelled
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
                    res.died = true;
                    return res;
                }
                s.y = o.r.getMaxY() + half;
                s.vy = 0.f;
                s.grounded = floorHit;
            } else if (fromBelow) {
                bool floorHit = g < 0.f;
                if (!floorHit && !both) {
                    res.died = true;
                    return res;
                }
                s.y = o.r.getMinY() - half;
                s.vy = 0.f;
                s.grounded = floorHit;
            } else {
                res.died = true; // ran into the side of a block
                return res;
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
                res.died = true;
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
