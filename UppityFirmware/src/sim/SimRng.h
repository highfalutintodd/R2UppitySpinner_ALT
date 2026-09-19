#pragma once
#include <cstdint>
// Deterministic xorshift RNG so simulator runs are reproducible from a seed.
namespace uppity { namespace sim {
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed = 0x9E3779B9u) : s(seed ? seed : 1u) {}
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    // Uniform in [0, 1).
    double unit() { return (next() >> 8) * (1.0 / 16777216.0); }
    bool chance(double p) { return unit() < p; }
};
}} // namespace uppity::sim
