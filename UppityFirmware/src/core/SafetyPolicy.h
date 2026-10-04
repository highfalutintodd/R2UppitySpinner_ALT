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
//
// Rule 2 has one exemption: a head at or below the safe-spin height is inside the dome opening
// and therefore aligned. That only holds if it came down past that height while home and has not
// turned since. It fails at the threshold itself (bench 2026-10-03: a turned head that rule 2
// clamped AT the safe-spin height was let down by the next jog press and hit the lifter frame),
// and the rescue override breaks it outright. So the supervisor passes `alignmentSuspect` (its
// turnedSinceHome_: the head was off home above the safe height, or turned below it, since its
// last home) and the exemption is off while it is set. The top limit switch is never "inside
// the opening", whatever the safe-spin setting says.
//
// And a creep that the rule-2 floor has already stopped (`clampHeld`: it came to rest at the
// safe-spin height with the head turned) may not try again: it brakes early so as to stop AT the
// height, and a second press from there, starting from rest, could not brake early enough. Every
// descent is refused until a home (or a rise of kClampRearmTicks, or a new safe-spin setting).
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
    bool  alignmentSuspect = false; // the head may be turned: off home above the safe height (or turned below it) since its last home
    bool  clampHeld = false;        // a creep was stopped at the safe-spin height with the head turned, and it has not homed or risen since
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
        if (rotaryIsHome(c) || c.overrideLive) return d;
        if (c.clampHeld) { d.verdict = Verdict::Refuse; d.reason = Reason::RotaryNotHome; return d; }
        // A head already at or below the safe-spin height is inside the dome opening and
        // therefore aligned (the rotary is refused down there); it may keep descending, unless
        // the override has since let it turn or come down unaligned.
        bool crossing = current > c.safeSpinTicks || c.topLimit;
        if (!crossing && c.alignmentSuspect) { d.verdict = Verdict::Refuse; d.reason = Reason::RotaryNotHome; return d; }
        Ticks floor = crossing ? c.safeSpinTicks : 0;
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
        if (rotaryIsHome(c) || c.overrideLive) return d;
        if (c.clampHeld) { d.verdict = Verdict::Refuse; d.reason = Reason::RotaryNotHome; return d; }
        bool crossing = c.lifter.ticks > c.safeSpinTicks || c.topLimit;
        if (!crossing && c.alignmentSuspect) { d.verdict = Verdict::Refuse; d.reason = Reason::RotaryNotHome; return d; }
        if (crossing) d.value = c.safeSpinTicks;                  // stop at the threshold
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
