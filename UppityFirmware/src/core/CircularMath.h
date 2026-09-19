#pragma once
#include <cstdint>
// Integer circular (0–359 degree) math. Adapted from Roam-A-Dome ALT's CircularMath.h
// (highfalutintodd/Roam-A-Dome_ALT, LGPL-2.1). Pure C++, host-testable, no floating point.
namespace uppity {

// Normalize any degree value into [0, 360).
constexpr int16_t normalizeDeg(int32_t deg) {
    int32_t d = deg % 360;
    return static_cast<int16_t>(d < 0 ? d + 360 : d);
}

// Signed shortest rotation from `from` to `to`, in (-180, 180].
constexpr int16_t signedCircularDelta(int16_t from, int16_t to) {
    int32_t d = normalizeDeg(to) - normalizeDeg(from);
    if (d > 180) d -= 360;
    else if (d <= -180) d += 360;
    return static_cast<int16_t>(d);
}

// Unsigned shortest angular distance, in [0, 180].
constexpr int16_t circularDistance(int16_t a, int16_t b) {
    int16_t d = signedCircularDelta(a, b);
    return d < 0 ? static_cast<int16_t>(-d) : d;
}

// True if `pos` lies within the arc swept from `start` to `end` going in the increasing
// direction. Handles arcs spanning the 0/359 seam.
constexpr bool withinArc(int16_t start, int16_t end, int16_t pos) {
    int16_t s = normalizeDeg(start), e = normalizeDeg(end), p = normalizeDeg(pos);
    if (s <= e) return p >= s && p <= e;
    return p >= s || p <= e;
}

// Same idea in encoder ticks for a known ticks-per-revolution: shortest signed delta.
inline int32_t signedTickDelta(int32_t from, int32_t to, int32_t ticksPerRev) {
    if (ticksPerRev <= 0) return to - from;
    int32_t d = (to - from) % ticksPerRev;
    if (d > ticksPerRev / 2) d -= ticksPerRev;
    else if (d <= -ticksPerRev / 2) d += ticksPerRev;
    return d;
}
inline int32_t modTicks(int32_t t, int32_t ticksPerRev) {
    if (ticksPerRev <= 0) return t;
    int32_t m = t % ticksPerRev;
    return m < 0 ? m + ticksPerRev : m;
}

} // namespace uppity
