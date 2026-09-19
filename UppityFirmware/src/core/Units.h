#pragma once
#include <cstdint>
// Units and small helpers shared by the pure core. No Arduino here.
namespace uppity {

using Ticks = int32_t;     // encoder counts
using Ms    = uint32_t;    // milliseconds (wraps after 49 days; always compare by subtraction)
using Throttle = int16_t;  // signed drive, -1000..+1000 = -100.0 %..+100.0 %

constexpr Throttle kThrottleMax = 1000;

inline Throttle clampThrottle(int v) {
    if (v > kThrottleMax) return kThrottleMax;
    if (v < -kThrottleMax) return -kThrottleMax;
    return static_cast<Throttle>(v);
}
inline Throttle pctToThrottle(int pct) { return clampThrottle(pct * 10); }
inline int throttleToPct(Throttle t) { return (t + (t >= 0 ? 5 : -5)) / 10; }
inline int absi(int v) { return v < 0 ? -v : v; }
inline int32_t absl(int32_t v) { return v < 0 ? -v : v; }
inline int sgn(int v) { return (v > 0) - (v < 0); }
template <typename T> inline T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Elapsed time that survives millis() wrap.
inline Ms elapsed(Ms now, Ms since) { return now - since; }

} // namespace uppity
