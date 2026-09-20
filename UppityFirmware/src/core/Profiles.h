#pragma once
#include <cstdint>
#include "Units.h"
// Lifter motor profiles (data carried over from v3 unchanged, with the rationale) and the
// rotary characterisation model that replaces v3's unmodelled rotary.
namespace uppity {

enum class LifterMotor : uint8_t {
    Pololu4757_6p3 = 0,   // "Greg" 6.3:1, the original
    Pololu4751_19  = 1,   // 19:1 high-torque
    IAParts        = 2,
    Count
};

struct LifterMotorProfile {
    const char* name;
    uint8_t  defaultMinPowerPct;        // lowest % that reliably moves the lifter
    uint8_t  defaultSeekBottomPowerPct; // approach floor near a limit
    uint16_t defaultTravelTicks;        // fallback before calibration measures travel
    uint16_t maxThrottle;               // 0.1 % units; the ceiling normal commands may reach
    uint8_t  calibrationSweepCapPct;    // highest slot the calibration sweep tries
    uint16_t stallTimeoutMs;            // no encoder change for this long = stall
    uint16_t slowProgressMinTicksPerSec;
    uint16_t slowProgressMaxMs;
    uint8_t  pulseOnMs;                 // pulsed-cruise pattern (lead-screw soft stop)
    uint8_t  pulseOffMs;
    uint16_t breakawayMaxMs;            // continuous burst allowed to overcome static friction
    uint16_t breakawayRampTicksCap;
    uint16_t calibrationCooldownMs;
    uint16_t commissioningThrottle;     // 0.1 % units; raise-to-top speed for commissioning moves
    bool     softApproachEnabled;       // ramp down over the last softApproachPct of an upward seek
    uint8_t  softApproachPct;
};

// Values from R2UppitySpinnerV3.ino v3.6.0 lines 293–366. The 19:1 profile is deliberately
// capped at 75 % throttle / 85 % sweep with a 700 ms stall timeout: Greg Hulette cracked a
// lifter frame calibrating a 19:1 at full power. Expert mode lifts the caps to 100 %.
constexpr LifterMotorProfile kProfiles[static_cast<int>(LifterMotor::Count)] = {
    { "Pololu 4757 (6.3:1, original)", 65, 40,  450, 1000, 100, 2000, 10, 3000, 3, 1, 500, 80, 2500, 800, false,  0 },
    { "Pololu 4751 (19:1, high-torque)", 45, 30, 1350,  750,  85,  700, 25, 1500, 5, 2, 250, 40, 4000, 600, true,  15 },
    { "IA-Parts",                        30, 30,  845, 1000, 100, 2000, 10, 3000, 3, 1, 500, 80, 2500, 800, false,  0 },
};

inline const LifterMotorProfile& profileFor(LifterMotor m) {
    int i = static_cast<int>(m);
    if (i < 0 || i >= static_cast<int>(LifterMotor::Count)) i = 0;
    return kProfiles[i];
}
inline bool expertModeApplies(LifterMotor m) { return m == LifterMotor::Pololu4751_19; }
inline Throttle effectiveMaxThrottle(LifterMotor m, bool expert) {
    return (expert && expertModeApplies(m)) ? kThrottleMax : static_cast<Throttle>(profileFor(m).maxThrottle);
}
inline uint8_t effectiveSweepCapPct(LifterMotor m, bool expert) {
    return (expert && expertModeApplies(m)) ? 100 : profileFor(m).calibrationSweepCapPct;
}

// The rotary has no motor profile; every build is different (measured 250–1191 ticks per
// revolution). These are measured at commissioning and refined by every successful home.
struct RotaryCharacterisation {
    float    ticksPerRev = 0.f;         // 0 = unknown; absolute moves need a value
    bool     confident = false;         // spread <= 5 % over >= 2 revolutions
    uint16_t contactArcTicks = 0;       // width of the switch's conducting arc while moving
    uint8_t  chatterPerPass = 0;        // raw make/break count per pass (geometry, not wear)
    uint8_t  minPowerPct = 40;          // floor for normal rotary moves
    uint8_t  jogFloorPct = 12;          // floor for rescue jog (datasheet 12-88 % PWM band)
    uint16_t rampLatencyMs = 25;        // v3 ramp scheme, kept
    uint8_t  rampAccelScale = 100;
    uint8_t  rampDecelScale = 20;
    uint16_t stopDistanceTicks = 0;     // coast after brake at minPower, measured; 0 = unknown
    int8_t   driveSign = -1;            // +1: positive throttle increases the tick count; v3 wiring was -1
    uint16_t slewAccelPermille = 40;    // throttle change per rampLatencyMs while accelerating
    uint16_t slewDecelPermille = 120;   // while decelerating or reversing
    uint8_t  settleWiggleDeg = 2;       // homing pass 3: first swing of the decaying settle wiggle (Todd picked 2° on the bench); 0 = none
    bool     parkOnLeadingEdge = true;  // home = first switch closure met while creeping onto the arc, found with the switch alone
                                        // (v3's parking spot). Off = the arc centre by encoder. On is the only choice when the
                                        // count cannot be trusted tick by tick (Todd's drive, 2026-09-20).
    int8_t   parkApproach = -1;         // creep direction of that final approach relative to the sweep: -1 = against it (v3's
                                        // left-first search met the far edge of v4's sweep; Todd's head is aligned there), +1 = with it
    int16_t  homeOffsetTicks = 0;       // where the head looks straight, relative to the switch arc centre (v3 parked on the
                                        // arc's leading edge, so heads aligned under v3 sit ~half an arc from the centre)

    Ticks degToTicks(float deg) const { return ticksPerRev > 0 ? static_cast<Ticks>(deg * ticksPerRev / 360.f + (deg >= 0 ? 0.5f : -0.5f)) : 0; }
    float ticksToDeg(Ticks t) const { return ticksPerRev > 0 ? static_cast<float>(t) * 360.f / ticksPerRev : 0.f; }
};

} // namespace uppity
