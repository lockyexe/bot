#include "Humanizer.hpp"

#include <algorithm>
#include <cmath>

namespace pf {

namespace {

uint64_t splitmix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

constexpr float TAU = 6.28318530718f;

} // namespace

float Humanizer::hash01(uint64_t key) {
    return static_cast<float>(splitmix(key) >> 40) / static_cast<float>(1ull << 24);
}

HumanParams Humanizer::makeParams(uint64_t seed) {
    std::mt19937_64 rng(splitmix(seed));
    auto uni = [&](float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); };
    auto inti = [&](int a, int b) { return std::uniform_int_distribution<int>(a, b)(rng); };
    HumanParams p;
    p.baseFloor = inti(12, 16);
    p.microFloor = inti(2, 4);
    p.band = uni(10.f, 16.f);
    p.driftAmp = uni(6.f, 12.f);
    p.driftFreq = uni(0.007f, 0.016f);
    p.phaseA = uni(0.f, TAU);
    p.phaseB = uni(0.f, TAU);
    p.noiseSeed = static_cast<uint32_t>(rng());
    return p;
}

void Humanizer::reseed(uint64_t seed) {
    m_rng.seed(splitmix(seed ^ m_params.noiseSeed));
}

int Humanizer::minStateTicks(int spotFailures) const {
    return microAllowed(spotFailures) ? m_params.microFloor : m_params.baseFloor;
}

float Humanizer::perlin(float t) const {
    // Classic 1D gradient noise: random slope at each integer, smooth fade.
    auto grad = [&](int64_t i) {
        float h = hash01(static_cast<uint64_t>(i) * 0x2545F4914F6CDD1Dull + m_params.noiseSeed);
        return h * 2.f - 1.f;
    };
    auto i0 = static_cast<int64_t>(std::floor(t));
    float f = t - static_cast<float>(i0);
    float g0 = grad(i0) * f;
    float g1 = grad(i0 + 1) * (f - 1.f);
    float u = f * f * f * (f * (f * 6.f - 15.f) + 10.f);
    return (g0 + (g1 - g0) * u) * 2.f; // roughly -1..1
}

float Humanizer::drift(uint64_t tick, float corridorHeight) const {
    float t = static_cast<float>(tick);
    float s = 0.55f * std::sin(t * m_params.driftFreq + m_params.phaseA)
            + 0.25f * std::sin(t * m_params.driftFreq * 2.3f + m_params.phaseB)
            + 0.20f * perlin(t * m_params.driftFreq * 1.7f);
    // Never let the drift eat more than 15% of a tight corridor.
    float amp = std::min(m_params.driftAmp, std::max(0.f, corridorHeight * 0.15f));
    return s * amp;
}

float Humanizer::band(float corridorHeight, float scale) const {
    float b = m_params.band * scale;
    // Keep the band inside the corridor, but never collapse to 1-tick flying.
    float maxB = std::max(4.f, corridorHeight * 0.5f - 6.f);
    return std::clamp(b, 4.f, maxB);
}

std::vector<uint64_t> Humanizer::centeredOrder(uint64_t a, uint64_t b, int step) {
    std::vector<uint64_t> out;
    if (b < a) return out;
    step = std::max(step, 1);
    double len = static_cast<double>(b - a);
    double lo = static_cast<double>(a) + len * 0.45;
    double hi = static_cast<double>(a) + len * 0.65;
    for (uint64_t t = a; t <= b; t += static_cast<uint64_t>(step)) out.push_back(t);
    auto dist = [&](uint64_t t) {
        double d = static_cast<double>(t);
        if (d < lo) return lo - d;
        if (d > hi) return d - hi;
        return 0.0;
    };
    double mid = (lo + hi) * 0.5;
    std::stable_sort(out.begin(), out.end(), [&](uint64_t x, uint64_t y) {
        double dx = dist(x), dy = dist(y);
        if (dx != dy) return dx < dy;
        return std::abs(static_cast<double>(x) - mid) < std::abs(static_cast<double>(y) - mid);
    });
    return out;
}

uint64_t Humanizer::pickCentered(uint64_t a, uint64_t b) {
    if (b <= a) return a;
    double len = static_cast<double>(b - a);
    double f = std::uniform_real_distribution<double>(0.45, 0.65)(m_rng);
    return a + static_cast<uint64_t>(std::llround(len * f));
}

bool Humanizer::recentlyPeriodic() const {
    if (m_toggles.size() < 5) return false;
    std::vector<uint64_t> v(m_toggles.end() - 5, m_toggles.end());
    return periodicity(v) > 0.85;
}

int Humanizer::toggleJitter(bool safe) {
    if (!safe) return 0;
    bool periodic = recentlyPeriodic();
    // Mostly 0-1 tick of natural slop, 1-3 when the rhythm got robotic.
    int lo = periodic ? 1 : 0;
    int hi = periodic ? 3 : 1;
    return std::uniform_int_distribution<int>(lo, hi)(m_rng);
}

void Humanizer::noteToggle(uint64_t tick) {
    m_toggles.push_back(tick);
    while (m_toggles.size() > 16) m_toggles.pop_front();
}

double Humanizer::periodicity(std::vector<uint64_t> const& ticks) {
    if (ticks.size() < 4) return 0.0;
    std::vector<double> iv;
    for (size_t i = 1; i < ticks.size(); i++) {
        if (ticks[i] <= ticks[i - 1]) continue;
        iv.push_back(static_cast<double>(ticks[i] - ticks[i - 1]));
    }
    if (iv.size() < 3) return 0.0;
    // Toggles alternate press/release, so compare every second interval
    // (press->press) as well as consecutive ones.
    auto score = [](std::vector<double> const& v) {
        if (v.size() < 2) return 0.0;
        double mean = 0;
        for (double x : v) mean += x;
        mean /= static_cast<double>(v.size());
        if (mean <= 0) return 0.0;
        double var = 0;
        for (double x : v) var += (x - mean) * (x - mean);
        double sd = std::sqrt(var / static_cast<double>(v.size()));
        // sd <= 0.5 tick reads as machine-perfect.
        return std::clamp(1.0 - (sd - 0.5) / 2.5, 0.0, 1.0);
    };
    std::vector<double> pairs;
    for (size_t i = 1; i < iv.size(); i += 2) pairs.push_back(iv[i - 1] + iv[i]);
    return std::max(score(iv), score(pairs));
}

} // namespace pf
