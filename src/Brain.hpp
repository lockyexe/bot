#pragma once

// Brain: a learned model of Geometry Dash physics plus a small simulator.
//
// PhysicsModel watches every physics tick the bot plays and learns, per
// game mode and size, how the vertical speed changes (gravity, ship thrust,
// robot boost), how strong a jump/UFO tap is, terminal speeds and the wave
// slope. It starts from rough priors and converges within a few jumps; the
// learned numbers are saved between sessions.
//
// Simulator rolls the player forward tick by tick under a given input
// pattern against the real hitboxes ahead (hazards kill, blocks are landed
// on or slid along depending on the mode, side hits kill). Orbs and pads
// use effects learned from watching them being used (until one is learned,
// reaching it ends the rollout as "uncertain"). Gravity, size, mode and speed
// portals are applied; deaths after a mode change count as uncertain.
// Dash/teleport/spider orbs, dual/teleport portals and slopes are not
// modelled and end the rollout as uncertain.
//
// Everything is in physics ticks (240 per second of game time).

#include <Geode/Geode.hpp>

#include <functional>
#include <string>
#include <vector>

#include "Types.hpp"

namespace pf {

struct SimObj {
    cocos2d::CCRect r;
    ObjKind kind = ObjKind::Solid;
    bool slope = false;
    int type = -1;     // GameObjectType value (orbs, pads, portals)
    float speed = 0.f; // speed portals: new horizontal speed, units per tick at that speed
};

// What touching/clicking an orb or pad does, gravity-relative after any flip.
struct SpecialEffect {
    bool known = false;
    bool prior = false; // an educated guess (never seen it used yet)
    bool flip = false;
    float vyRel = 0.f;
};

struct SimWorld {
    std::vector<SimObj> objs; // sorted by minX
    float floorY = 90.f;      // ground, or the mode's floor
    float ceilY = 1e9f;       // the mode's ceiling (1e9 = none)
};

struct SimState {
    float x = 0, y = 0;
    float vy = 0;  // world units per tick
    float vx = 0;  // world units per tick
    float w = 30, h = 30;
    bool grounded = false;
    bool upside = false;
    bool mini = false;
    bool hold = false; // input state before the first simulated tick
    Mode mode = Mode::Cube;
};

struct SimResult {
    bool died = false;
    bool uncertain = false; // reached something the model doesn't simulate
    int ticks = 0;          // ticks simulated (death tick when died)
};

// Hold state at simulated tick i (decision made i ticks after the start).
using InputFn = std::function<bool(int)>;

class PhysicsModel {
public:
    PhysicsModel();

    // One observed physics tick. Velocities are world-space, per tick.
    void observe(Mode mode, bool mini, bool upside, bool holdApplied, bool groundedBefore, bool groundedAfter,
                 float vyBefore, float vyAfter, float vx, int ticksSincePress);

    float accel(Mode m, bool mini, bool hold, bool ascending) const; // gravity-relative Δvy per tick
    float impulse(Mode m, bool mini) const;                          // gravity-relative vy right after a jump/tap
    float vmaxUp(Mode m, bool mini) const;
    float vmaxDown(Mode m, bool mini) const; // negative
    float waveSlope(bool mini) const;        // |vy| / vx

    // Orbs and pads, learned per object type, mode and size.
    SpecialEffect special(int type, Mode m, bool mini) const;
    void observeSpecial(int type, Mode m, bool mini, float vyRelAfter);
    static bool flipsGravity(int type);
    static bool unsupported(int type);
    int learnedSpecials() const;

    // Self-correction per mode: hazards get a safety margin that grows when
    // a "safe" prediction ends in a death, and a mode whose predictions keep
    // failing stops being trusted.
    float hazardMargin(Mode m) const { return m_margin[mi(m)]; }
    void noteMiss(Mode m);
    void noteHit(Mode m);
    bool trusted(Mode m) const;

    void noteAccuracy(bool ok);
    float accuracy() const { return m_accuracy; }
    int accuracySamples() const { return m_accN; }
    int samples() const { return m_samples; }

    std::string serialize() const;
    void deserialize(std::string const& s);

private:
    struct Stat {
        float value = 0;
        int n = 0;
        void add(float v, float alpha);
    };
    static int mi(Mode m) { return static_cast<int>(m); }

    Stat m_accel[8][2][2][2]; // mode, mini, hold, ascending
    Stat m_impulse[8][2];
    Stat m_vUp[8][2];
    Stat m_vDown[8][2];
    Stat m_wave[2];
    Stat m_special[48][8][2];
    float m_margin[8] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
    int m_hits[8] = {};
    int m_misses[8] = {};
    float m_accuracy = 0.f;
    int m_accN = 0;
    int m_samples = 0;
};

class Simulator {
public:
    static SimResult run(PhysicsModel const& m, SimState s, SimWorld const& w, InputFn const& input, int horizon);
};

} // namespace pf
