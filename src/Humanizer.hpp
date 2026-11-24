#pragma once

// Humanizer: everything that makes the solver's inputs look like a person
// played them. The search (Pathfinder) asks this class how long a click must
// at least last, which tick inside an activation window to try first, where
// the "comfortable" line through a flying corridor drifts to, and whether a
// toggle can be nudged by a few ticks to break up robotic rhythm.
//
// All randomness is seeded, so the same segment + attempt produces the same
// choices. Inputs that were already played are stored as events by the
// Pathfinder and replayed exactly, so nothing here has to be reproducible
// beyond a single attempt.

#include <cstdint>
#include <deque>
#include <random>
#include <vector>

namespace pf {

// Per-checkpoint "personality": picked once when a segment is created so
// one stretch of the level feels consistent, while different stretches
// differ slightly (like a real player's hands warming up).
struct HumanParams {
    int baseFloor = 14;       // minimum hold/release length, 12-16 ticks
    int microFloor = 3;       // relaxed floor for proven micro-click spots, 2-4 ticks
    float band = 13.f;        // hysteresis half-width around the corridor line, 10-16 units
    float driftAmp = 9.f;     // amplitude of the slow Perlin/sine drift
    float driftFreq = 0.011f; // drift speed (per tick)
    float phaseA = 0.f;
    float phaseB = 0.f;
    uint32_t noiseSeed = 1;
};

class Humanizer {
public:
    // New random personality for a segment.
    static HumanParams makeParams(uint64_t seed);

    void setParams(HumanParams const& p) { m_params = p; }
    HumanParams const& params() const { return m_params; }

    // Seed for this attempt (jitter choices etc.).
    void reseed(uint64_t seed);

    // Shortest a hold or a release may last before toggling again.
    // Only drops to the micro floor after a spot failed more than 15 times.
    int minStateTicks(int spotFailures) const;
    int microTicks() const { return m_params.microFloor; }
    bool microAllowed(int spotFailures) const { return spotFailures > 15; }

    // Slow organic offset (units) for the corridor centre line.
    float drift(uint64_t tick, float corridorHeight) const;
    float band(float corridorHeight, float scale) const;

    // Ticks in [a, b] ordered so the 45%-65% middle of the window comes
    // first, then spreading outwards. `step` thins the list.
    static std::vector<uint64_t> centeredOrder(uint64_t a, uint64_t b, int step = 1);

    // A tick inside [a, b] picked in the 45%-65% zone.
    uint64_t pickCentered(uint64_t a, uint64_t b);

    // Extra delay (0-3 ticks) to apply to a toggle the controller just
    // decided on. 0 when not safe. Bigger when recent toggles became
    // suspiciously regular.
    int toggleJitter(bool safe);

    // Remember a toggle that really happened (for the rhythm check).
    void noteToggle(uint64_t tick);
    void clearHistory() { m_toggles.clear(); }

    // 0 = irregular (good) ... 1 = perfectly periodic (robotic).
    static double periodicity(std::vector<uint64_t> const& toggleTicks);

    // Deterministic 0..1 value for a key (used to give each obstacle its own
    // preferred spot inside its window).
    static float hash01(uint64_t key);

private:
    float perlin(float t) const;
    bool recentlyPeriodic() const;

    HumanParams m_params;
    std::mt19937_64 m_rng{1};
    std::deque<uint64_t> m_toggles;
};

} // namespace pf
