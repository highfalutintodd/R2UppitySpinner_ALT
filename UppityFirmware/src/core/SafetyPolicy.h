#pragma once
#include "Units.h"
#include "Knowledge.h"
#include "EventLog.h"
#include "CircularMath.h"
// The safety invariants, evaluated in exactly one place. Every job asks here before it starts
// and the supervisor re-asks on every tick. Pure functions over a context snapshot.
//
//  1. The rotary may not turn unless the lifter is known to be above the safe-spin height (or
//     the top limit is physically made). Rescue jog may override this.
//  2. The lifter may not descend below the safe-spin height unless the rotary is home. Rescue
//     override may bypass this.
//  3. With the lifter position UNKNOWN only upward motion is allowed. No override.
//  4. E-STOP latched, commissioning lock, or a blocking fault refuses everything.
namespace uppity {

struct PolicyContext {
    LifterKnowledge lifter;
    RotaryKnowledge rotary;
    float ticksPerRev = 0.f;
    Ticks safeSpinTicks = 0;        // lifter height above which the head clears the dome
    Ticks homeOffsetTicks = 0;      // visual home relative to the switch centre (frame zero)
    bool  rotaryDisabled = false;
    bool  estop = false;
    bool  locked = false;           // commissioning incomplete
    bool  lifterFault = false;      // blocking faults per axis (from FaultModel)
    bool  rotaryFault = false;
    bool  globalFault = false;
    bool  overrideLive = false;     // rescue override lease
    bool  topLimit = false, bottomLimit = false, homeSwitch = false;
};

enum class Verdict : uint8_t { Allow, Clamp, Refuse };
struct Decision {
    Verdict verdict = Verdict::Allow;
    Reason  reason = Reason::None;
    Ticks   value = 0;              // Clamp: the clamped target / floor
    bool ok() const { return verdict != Verdict::Refuse; }
};

inline Ticks homeToleranceTicks(float ticksPerRev) {
    return ticksPerRev > 0 ? static_cast<Ticks>(ticksPerRev * kHomeToleranceDeg / 360.f + 0.5f) : 0;
}

// "Is the head aligned with the dome opening?" — the only definition in the firmware.
inline bool rotaryIsHome(const PolicyContext& c) {
    if (c.rotaryDisabled) return true;
    switch (c.rotary.confidence) {
        case HomeConfidence::SwitchLive: return absl(c.homeOffsetTicks) <= homeToleranceTicks(c.ticksPerRev > 0 ? c.ticksPerRev : 1184.f);   // on the switch = at the centre
        case HomeConfidence::Latched: {
            if (c.ticksPerRev <= 0) return false;
            Ticks tpr = static_cast<Ticks>(c.ticksPerRev + 0.5f);
            Ticks d = signedTickDelta(c.homeOffsetTicks, c.rotary.ticks, tpr);
            return absl(d) <= homeToleranceTicks(c.ticksPerRev);
        }
        default: return false;
    }
}

inline bool lifterAboveSafe(const PolicyContext& c) {
    if (c.topLimit) return true;
    return c.lifter.confidence != LifterConfidence::Unknown && c.lifter.ticks > c.safeSpinTicks;
}

inline Decision gateCommon(const PolicyContext& c) {
    Decision d;
    if (c.estop)       { d.verdict = Verdict::Refuse; d.reason = Reason::Estop; }
    else if (c.locked) { d.verdict = Verdict::Refuse; d.reason = Reason::Locked; }
    else if (c.globalFault) { d.verdict = Verdict::Refuse; d.reason = Reason::DiagFault; }
    return d;
}

// Lifter seek from `current` to `target` ticks.
inline Decision checkLifterSeek(const PolicyContext& c, Ticks current, Ticks target) {
    Decision d = gateCommon(c);
    if (!d.ok()) return d;
    if (c.lifterFault) { d.verdict = Verdict::Refuse; d.reason = Reason::DiagFault; return d; }
    if (target < current) {
        if (c.lifter.confidence == LifterConfidence::Unknown) { d.verdict = Verdict::Refuse; d.reason = Reason::LifterUnknown; return d; }
        // A head already at or below the safe-spin height is inside the dome opening and
        // therefore aligned (the rotary is refused down there); it may keep descending.
        bool crossing = current > c.safeSpinTicks;
        Ticks floor = (rotaryIsHome(c) || c.overrideLive || !crossing) ? 0 : c.safeSpinTicks;
        if (target < floor) {
            d.verdict = Verdict::Clamp; d.reason = Reason::RotaryNotHome; d.value = floor; return d;
        }
    }
    return d;
}

// Lifter creep/jog in direction dir (+1 up, -1 down). value = floor the machine must stop at.
inline Decision checkLifterCreep(const PolicyContext& c, int dir) {
    Decision d = gateCommon(c);
    if (!d.ok()) return d;
    if (c.lifterFault) { d.verdict = Verdict::Refuse; d.reason = Reason::DiagFault; return d; }
    d.value = -2147483647;
    if (dir < 0) {
        if (c.lifter.confidence == LifterConfidence::Unknown) { d.verdict = Verdict::Refuse; d.reason = Reason::LifterUnknown; return d; }
        if (!(rotaryIsHome(c) || c.overrideLive) && c.lifter.ticks > c.safeSpinTicks) d.value = c.safeSpinTicks;   // stop at the threshold
    }
    return d;
}

// Any rotary motion. jog = rescue jog (override may bypass the height rule).
inline Decision checkRotary(const PolicyContext& c, bool jog) {
    Decision d = gateCommon(c);
    if (!d.ok()) return d;
    if (c.rotaryDisabled) { d.verdict = Verdict::Refuse; d.reason = Reason::RotaryDisabled; return d; }
    if (c.rotaryFault) { d.verdict = Verdict::Refuse; d.reason = Reason::DiagFault; return d; }
    if (!lifterAboveSafe(c) && !(jog && c.overrideLive)) { d.verdict = Verdict::Refuse; d.reason = Reason::BelowSafeHeight; return d; }
    return d;
}

} // namespace uppity
