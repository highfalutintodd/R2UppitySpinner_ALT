#pragma once
#include <cstdint>
#include "Units.h"
// Faults, their debouncers, and their clearing rules (docs/REWRITE-PLAN.md Appendix F.6).
// DIAG is read only while the drivers are enabled and the enable is at least kDiagArmMs old,
// because the TB9051FTG pulls DIAG low whenever it is disabled and during the ~80 us initial
// diagnosis after every enable (docs/TB9051FTG-NOTES.md).
namespace uppity {

enum Fault : uint32_t {
    F_None             = 0,
    F_LifterDiag       = 1u << 0,
    F_RotaryDiag       = 1u << 1,
    F_DiagRepeated     = 1u << 2,   // latching: >= 3 DIAG trips in 60 s
    F_LifterStall      = 1u << 3,
    F_RotaryStall      = 1u << 4,
    F_EncoderDirection = 1u << 5,
    F_EncoderNoise     = 1u << 6,
    F_LimitContradiction = 1u << 7, // latching while present
    F_LimitMismatch    = 1u << 8,
    F_HomeSwitchStuck  = 1u << 9,
    F_Expander         = 1u << 10,
    F_TickOverrun      = 1u << 11,
    F_PersistDeferred  = 1u << 12,
};

class FaultModel {
  public:
    static constexpr int  kDiagDebounceTicks = 2;
    static constexpr Ms   kDiagArmMs = 3;          // after enable ack, before DIAG is believed
    static constexpr Ms   kDiagClearMs = 100;      // DIAG high this long clears the fault
    static constexpr int  kDiagRepeatLimit = 3;
    static constexpr Ms   kDiagRepeatWindowMs = 60000;
    static constexpr int  kLimitDebounceTicks = 3;
    static constexpr Ms   kEnCycleLowMs = 50;      // EN held low to clear the driver's latch

    // Per-tick update of the hardware-derived faults.
    void updateDiag(bool lifterDiagRaw, bool rotaryDiagRaw, bool enabled, Ms sinceEnableMs, Ms now) {
        bool armed = enabled && sinceEnableMs >= kDiagArmMs;
        updateOneDiag(lifterDiagRaw, armed, now, F_LifterDiag, lifterLow_, lifterHighSince_);
        updateOneDiag(rotaryDiagRaw, armed, now, F_RotaryDiag, rotaryLow_, rotaryHighSince_);
    }
    void updateLimits(bool top, bool bottom) {
        if (top && bottom) { if (both_ < 255) ++both_; } else both_ = 0;
        if (both_ >= kLimitDebounceTicks) mask_ |= F_LimitContradiction;
        else if (!(top && bottom)) mask_ &= ~static_cast<uint32_t>(F_LimitContradiction);
    }

    void set(Fault f) { mask_ |= f; }
    void clear(Fault f) { mask_ &= ~static_cast<uint32_t>(f); }
    bool has(Fault f) const { return (mask_ & f) != 0; }
    uint32_t mask() const { return mask_; }

    bool lifterBlocked() const { return has(F_LifterDiag) || has(F_LimitContradiction) || globalBlocked(); }
    bool rotaryBlocked() const { return has(F_RotaryDiag) || globalBlocked(); }
    bool globalBlocked() const { return has(F_DiagRepeated) || has(F_Expander); }

    // Operator "clear fault": drops the latching faults and the trip history.
    void operatorClear() {
        mask_ &= ~static_cast<uint32_t>(F_DiagRepeated | F_LifterStall | F_RotaryStall | F_EncoderDirection |
                                        F_EncoderNoise | F_LimitMismatch | F_HomeSwitchStuck | F_Expander);
        tripCount_ = 0;
    }

    // A DIAG trip asks the supervisor for an EN cycle (rising edge of EN clears the driver's
    // over-current / over-temperature latch). The supervisor owns the cycle itself.
    bool takeEnCycleRequest() { bool r = enCycleRequested_; enCycleRequested_ = false; return r; }

    uint32_t diagTrips() const { return tripCount_; }

  private:
    void updateOneDiag(bool low, bool armed, Ms now, Fault f, uint8_t& lowTicks, Ms& highSince) {
        if (!armed) { lowTicks = 0; highSince = now; return; }
        if (low) {
            if (lowTicks < 255) ++lowTicks;
            if (lowTicks >= kDiagDebounceTicks && !has(f)) {
                mask_ |= f;
                enCycleRequested_ = true;
                noteTrip(now);
            }
            highSince = now;
        } else {
            lowTicks = 0;
            if (has(f) && elapsed(now, highSince) >= kDiagClearMs) mask_ &= ~static_cast<uint32_t>(f);
        }
    }
    void noteTrip(Ms now) {
        if (tripCount_ == 0 || elapsed(now, firstTripAt_) > kDiagRepeatWindowMs) { tripCount_ = 0; firstTripAt_ = now; }
        ++tripCount_;
        if (tripCount_ >= static_cast<uint32_t>(kDiagRepeatLimit)) mask_ |= F_DiagRepeated;
    }

    uint32_t mask_ = 0;
    uint8_t lifterLow_ = 0, rotaryLow_ = 0, both_ = 0;
    Ms lifterHighSince_ = 0, rotaryHighSince_ = 0;
    bool enCycleRequested_ = false;
    uint32_t tripCount_ = 0;
    Ms firstTripAt_ = 0;
};

} // namespace uppity
