#pragma once
#include <cstdint>
#include "Units.h"
// What the firmware believes about where the mechanism is, and how much it trusts that.
// See docs/REWRITE-PLAN.md Appendix F.4.
namespace uppity {

enum class LifterConfidence : uint8_t {
    Unknown = 0,   // no limit switch has been touched this power cycle: up-only
    Known   = 1,   // referenced at a limit and the encoder has been continuous since
    Stale   = 2,   // a stall or an implausible encoder event since the last reference
};

enum class HomeConfidence : uint8_t {
    Unknown    = 0,  // no reference this power cycle
    SwitchLive = 1,  // the switch reads closed at rest right now (trusted: the cam only conducts near centre)
    Latched    = 2,  // an edge was captured this power cycle; position is |deg| within tolerance
    Stale      = 3,  // a latch exists but a stall/fault/implausible encoder happened since
};

enum class HomeProvenance : uint8_t { None = 0, EdgeCentre = 1, RestContact = 2, Manual = 3 };   // Manual: "set rotary home here" (Rescue)

constexpr int kHomeToleranceDeg = 10;   // v3.6.0's ROTARY_HOME_TOLERANCE_DEG, kept

struct LifterKnowledge {
    Ticks ticks = 0;
    LifterConfidence confidence = LifterConfidence::Unknown;
    Ticks travelTicks = 0;         // 0 = uncalibrated (profile default is used for %)
    bool  travelCalibrated = false;
    Ms    referencedAt = 0;
};

struct RotaryKnowledge {
    Ticks ticks = 0;
    HomeConfidence confidence = HomeConfidence::Unknown;
    HomeProvenance provenance = HomeProvenance::None;
    Ticks lastEdgeArcTicks = 0;
    uint8_t lastEdgeClosures = 0;
    Ms    referencedAt = 0;
};

} // namespace uppity
