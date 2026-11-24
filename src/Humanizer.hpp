#pragma once

// Humanizer: everything that makes the solver's inputs look like a person
// played them, per play-style profile:
//
//   Mode 1 - Human            12-16 tick holds, ±10-16 corridor band,
//                             45-65% window bias, 0-3 tick jitter
//   Mode 2 - Prodigy 15CPS    6-8 tick holds, ±2.5-5 band with damped
//                             curves, 35-55% bias ±1 tick, 15 CPS cap
//   Mode 3 - Unreal ILL 15CPS 1-tick releases allowed, ±0.8-1.5 band,
//                             no jitter / no centering, exact frame
//                             sweeps on stuck obstacles, 15 CPS cap
//
// Every duration here is in physics ticks (240 per second of game time),
// never wall-clock time, so the bot plays the same at 1.0x or with any
// speedhack.

#include <cstdint>
#include <deque>
#include <random>
#include <vector>

namespace pf {

enum class Profile : int { Human = 1, Prodigy = 2, Unreal = 3 };

char const* profileLabel(Profile p); // "1-Human", "2-Prodigy 15CPS", ...
Profile nextProfile(Profile p);      // 1 -> 2 -> 3 -> 1

// Fixed per-profile tuning (the ranges a segment's personality is drawn from).
struct ProfileSpec {
    int floorMin, floorMax;         // shortest hold/release, ticks
    int microMin, microMax;         // micro hold for stubborn spots, ticks
    float bandMin, bandMax;         // corridor hysteresis half-width, units
    float bandClamp;                // smallest band ever used
    float driftMin, driftMax;       // drift amplitude of the corridor line
    double centerLo, centerHi;      // timing bias inside an activation window
    bool centered;                  // false: plain chronological sweep
    int jitterVariance;             // 0 none, 1 = ±1 tick, 3 = human 0-3
    float vyDamping;                // 1 = raw speed, <1 = smoothed (curvier)
    int pressCooldown;              // min ticks between two presses (0 = off)
    uint64_t cpMin, cpMax;          // auto-checkpoint interval, ticks
    int maxFailures;                // 0 = use the setting
    bool frameSweep;                // exact -8..+8 tick sweep on stuck spots
};

ProfileSpec const& profileSpec(Profile p);

// Per-checkpoint "personality", drawn from the profile's ranges once when a
// segment is created so one stretch of the level feels consistent.
struct HumanParams {
    Profile profile = Profile::Human;
    int baseFloor = 14;
    int microFloor = 3;
    float band = 13.f;
    float driftAmp = 9.f;
    float driftFreq = 0.011f;
    float phaseA = 0.f;
    float phaseB = 0.f;
    uint32_t noiseSeed = 1;
};

class Humanizer {
public:
    static HumanParams makeParams(uint64_t seed, Profile profile);

    void setParams(HumanParams const& p) { m_params = p; }
    HumanParams const& params() const { return m_params; }
    ProfileSpec const& spec() const { return profileSpec(m_params.profile); }

    void reseed(uint64_t seed);

    // Shortest a hold or a release may last before toggling again.
    int minStateTicks(int spotFailures) const;
    int microTicks() const { return m_params.microFloor; }
    bool microAllowed(int spotFailures) const;
    int pressCooldown() const { return spec().pressCooldown; }

    float drift(uint64_t tick, float corridorHeight) const;
    float band(float corridorHeight, float scale) const;
    float vyDamping() const { return spec().vyDamping; }

    // Ticks in [a, b] in the order the profile wants to try them:
    // Human 45-65% zone first, Prodigy 35-55% zone first with ±1 tick
    // variance, Unreal plain chronological. `step` thins the list.
    static std::vector<uint64_t> orderWindow(HumanParams const& p, uint64_t a, uint64_t b, int step = 1,
                                             uint64_t salt = 0);
    static std::vector<uint64_t> centeredOrder(uint64_t a, uint64_t b, int step, double lo, double hi);

    // Extra delay to apply to a toggle the controller just decided on.
    int toggleJitter(bool safe);

    void noteToggle(uint64_t tick);
    void clearHistory() { m_toggles.clear(); }

    // 0 = irregular (good) ... 1 = perfectly periodic (robotic).
    static double periodicity(std::vector<uint64_t> const& toggleTicks);

    static float hash01(uint64_t key);

    // Natural tap length for a quick click (where hold length doesn't change
    // the physics). Humans don't tap for the exact same time every click:
    // Mode 1 ~50-90 ms, Mode 2 ~25-55 ms, Mode 3 exact 1 tick.
    static int tapLength(HumanParams const& p, uint64_t key);

    // Where inside a learned window [lo, hi] this profile likes to click.
    // Returns hi + 1 when the profile wants exact timing (no relocation).
    static uint64_t humanSpot(HumanParams const& p, uint64_t lo, uint64_t hi, uint64_t key);

private:
    float perlin(float t) const;
    bool recentlyPeriodic() const;

    HumanParams m_params;
    std::mt19937_64 m_rng{1};
    std::deque<uint64_t> m_toggles;
};

} // namespace pf
