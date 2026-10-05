#pragma once
#include "Units.h"
#include "Profiles.h"
#include "Knowledge.h"
#include "MotionTypes.h"
// Lifter state machines (docs/REWRITE-PLAN.md Appendix F.3). Pure: no Arduino, no HAL calls;
// the supervisor feeds TickInputs and writes the BridgeCommand each machine returns.
namespace uppity {

// ---------------------------------------------------------------- boot position resolution

struct BootResolution {
    LifterConfidence lifter = LifterConfidence::Unknown;
    Ticks lifterTicks = 0;
    bool contradiction = false;      // both limits made: wiring/polarity fault
    bool rotaryRestContact = false;  // home switch closed at rest: a trusted zero
};

inline BootResolution bootResolve(bool top, bool bottom, bool homeSwitch, Ticks travel) {
    BootResolution r;
    if (top && bottom) { r.contradiction = true; return r; }
    if (bottom) { r.lifter = LifterConfidence::Known; r.lifterTicks = 0; }
    else if (top) { r.lifter = LifterConfidence::Known; r.lifterTicks = travel; }
    r.rotaryRestContact = homeSwitch;
    return r;
}

// ---------------------------------------------------------------- calibrated braking

constexpr int kSlots = 21;   // calibration slots: 0, 5, 10 ... 100 %

// The calibration's stopping distances as braking TIMES against the speed they were measured at:
// for each 5 % slot, the pulsed cruise speed (ticks/s) and how far the mast ran on after the brake
// (ticks). On Todd's 19:1 (#PCAL 2026-10-04) the time grows with speed, 27 -> 38 ms going down and
// 5 -> 28 ms going up, so a seek looks up the time at the speed it has actually reached instead of
// the run-on of the speed it was asked for (code review 2026-10, D5).
struct BrakeTable {
    uint16_t vel[kSlots] = {};   // ticks per second, ascending
    uint8_t  ms[kSlots] = {};    // braking time at that speed
    uint8_t  n = 0;              // 0 = no calibration
    uint8_t  medianMs = 0;       // the median over the slots (what the creeps use)
    // Braking time at `tps` ticks per second: straight-line between the two measured speeds either
    // side of it, the nearest end's time outside them; 0 without a table.
    int msAt(int32_t tps) const {
        if (n == 0) return 0;
        if (tps <= vel[0]) return ms[0];
        for (int i = 0; i + 1 < n; ++i) {
            if (tps >= vel[i + 1]) continue;
            int32_t span = vel[i + 1] - vel[i];
            if (span <= 0) return ms[i];
            return ms[i] + static_cast<int>((static_cast<int32_t>(ms[i + 1]) - ms[i]) * (tps - vel[i]) / span);
        }
        return ms[n - 1];
    }
};
constexpr int kBrakeTableCapMs = 150;   // a slot that says more is not a braking time (same cap as brakeLeadMs)

// One wild slot (a bump, a slipped count) must not make the seeks at that speed brake far too early
// or too late: each slot is held within half and twice the median, the median itself as robust as
// brakeLeadMs() is for the creeps. Todd's table (27-38 ms down, 5-28 up) is untouched but for the
// slowest slot up (5 -> 10 ms: a slow rise brakes about 1.5 ticks earlier).
inline BrakeTable makeBrakeTable(const uint16_t over[kSlots], const uint16_t vel[kSlots]) {
    BrakeTable t;
    for (int i = 0; i < kSlots; ++i) {
        if (vel[i] == 0 || over[i] == 0) continue;       // a slot that measured no run-on is not a measurement
        int m = static_cast<int>((static_cast<int32_t>(over[i]) * 1000 + vel[i] / 2) / vel[i]);
        uint16_t v = vel[i]; uint8_t ms = static_cast<uint8_t>(m > kBrakeTableCapMs ? kBrakeTableCapMs : m);
        int j = t.n;                                     // insertion by speed
        while (j > 0 && t.vel[j - 1] > v) { t.vel[j] = t.vel[j - 1]; t.ms[j] = t.ms[j - 1]; --j; }
        t.vel[j] = v; t.ms[j] = ms; ++t.n;
    }
    if (t.n == 0) return t;
    uint8_t sorted[kSlots];
    for (int i = 0; i < t.n; ++i) sorted[i] = t.ms[i];
    for (int i = 1; i < t.n; ++i) { uint8_t x = sorted[i]; int j = i - 1; while (j >= 0 && sorted[j] > x) { sorted[j + 1] = sorted[j]; --j; } sorted[j + 1] = x; }
    int med = (t.n % 2) ? sorted[t.n / 2] : (sorted[t.n / 2 - 1] + sorted[t.n / 2] + 1) / 2;
    t.medianMs = static_cast<uint8_t>(med);
    for (int i = 0; i < t.n; ++i) t.ms[i] = static_cast<uint8_t>(clampv<int>(t.ms[i], (med + 1) / 2, med * 2 > kBrakeTableCapMs ? kBrakeTableCapMs : med * 2));
    return t;
}

constexpr Ticks kNoFloor = -2147483647;   // no policy floor (the head is home, the override, an ascent)

// ---------------------------------------------------------------- seek

struct SeekParams {
    Ticks    target = 0;
    Throttle speed = 0;             // already floored/capped by the supervisor
    Throttle approachPower = 300;   // soft-approach floor (profile seek-bottom power)
    Throttle minPower = 0;          // profile/tuned minimum reliable power; 0 = approachPower
    bool     fullTravel = false;    // seeking to a limit: ignore the encoder target
    BrakeTable brake;               // the calibration's braking times for this direction (none = brake on the mark)
    Ticks    floorTicks = kNoFloor; // descending with the head maybe turned: come to REST at or above this (the safe-spin height)
    const LifterMotorProfile* prof = nullptr;
};

// A seek brakes `ahead` ticks before its target: the speed over the last kVelWindow ms times the
// braking time the calibration gives at that speed, so a short step that never got up to speed
// brakes late and a fast one early. Before (code review D5) it braked within the run-on of its full
// speed from the first tick: 56 ticks going down on Todd's lifter, so a 33-tick :PP never moved and
// longer steps stopped up to that much early. When the policy has a floor for the descent (the head
// may be turned), it also brakes against that floor the way the creeps do: a higher speed estimate,
// the longer of the table's time and its median (kUncalibratedBrakeMs with no table), and a
// quarter and a tick more, whether the target was clamped to the floor or lies just above it.
class LifterSeek {
  public:
    enum class Phase : uint8_t { Idle, Breakaway, Ramp, Cruise, Braking, Finished };
    static constexpr Ticks kBreakawayTicks = 10;
    static constexpr Ticks kSkipBreakawayBelow = 30;    // full-travel seeks only
    static constexpr Ms    kBrakeMs = 100;
    static constexpr Ticks kAgainstDirLimit = 8;
    static constexpr Ticks kSlowRemainingMin = 20;
    static constexpr int   kVelWindow = 16;             // ms of position history for the speed estimate
    // No table: the longest braking time measured on any lifter so far (Todd's 19:1 going down at
    // full speed, ~40 ms) and half as much again. Used only against the safe-spin floor.
    static constexpr int   kUncalibratedBrakeMs = 60;

    void start(const SeekParams& p, Ticks current, Ms now) {
        p_ = p;
        dir_ = p.target >= current ? 1 : -1;
        if (p.target == current) dir_ = p.fullTravel ? 1 : 1;
        start_ = current; startMs_ = now;
        distance_ = absl(p.target - current);
        lastTicks_ = current; lastMoveMs_ = now;
        slowWindowStart_ = now; slowWindowTicks_ = current; slowAccumMs_ = 0; reboosted_ = false;
        pulse_.set(p.prof ? p.prof->pulseOnMs : 3, p.prof ? p.prof->pulseOffMs : 1);
        rampTicks_ = 0;
        for (Ticks& h : hist_) h = current;
        histIdx_ = 0; stoppedByFloor_ = false;
        // Short steps to a height run the breakaway burst too: pulsed power from rest starves a 19:1,
        // and they used to skip it under 30 ticks (harmlessly, as they braked at once). A short run
        // to a switch keeps the gentle start it always had.
        if (p.fullTravel && distance_ < kSkipBreakawayBelow) enter(Phase::Cruise, current, now);
        else enter(Phase::Breakaway, current, now);
        phaseChanged_ = true;
    }

    StepOutcome step(const TickInputs& in, BridgeCommand& out) {
        Ticks pos = in.lifterTicks;
        Ms now = in.nowMs;
        StepOutcome o;
        out = brakeCmd();
        if (phase_ == Phase::Finished) return pending_;
        if (universalExit(in, in.lifterDiag, o)) { finish(o); return pending_; }

        if (phase_ == Phase::Braking) {
            if (elapsed(now, brakeUntil_) < 0x80000000u && now != brakeUntil_ && static_cast<int32_t>(now - brakeUntil_) >= 0) { phase_ = Phase::Finished; return pending_; }
            return StepOutcome::running();
        }
        Ticks back = hist_[histIdx_], mid = hist_[(histIdx_ + kVelWindow / 2) % kVelWindow];   // where it was 16 and 8 ms ago
        hist_[histIdx_] = pos; histIdx_ = (histIdx_ + 1) % kVelWindow;

        // limit in the direction of travel
        if ((dir_ > 0 && in.topLimit) || (dir_ < 0 && in.bottomLimit)) { beginBrake(StepOutcome::done(JobResult::DoneLimit, Reason::LimitReached, pos), now); return StepOutcome::running(); }
        // the policy floor (a descent with the head maybe turned): come to rest at or above it
        if (dir_ < 0 && p_.floorTicks != kNoFloor && pos - floorAheadTicks(pos, mid, back) <= p_.floorTicks) {
            stoppedByFloor_ = true;
            beginBrake(StepOutcome::done(JobResult::DoneTarget, Reason::TargetReached, pos), now); return StepOutcome::running();
        }
        // target reached (not for full-travel seeks, which end on the switch)
        if (!p_.fullTravel) {
            Ticks ahead = aheadTicks(pos, mid, back);
            if ((dir_ > 0 && pos >= p_.target - ahead) || (dir_ < 0 && pos <= p_.target + ahead)) {
                beginBrake(StepOutcome::done(JobResult::DoneTarget, Reason::TargetReached, pos), now); return StepOutcome::running();
            }
        }
        // encoder moving against the drive
        if ((pos - start_) * dir_ < -kAgainstDirLimit) { finish(StepOutcome::failed(JobResult::Failed, Reason::EncoderDirection, pos)); return pending_; }
        // stall: no encoder change for the profile timeout
        if (pos != lastTicks_) { lastTicks_ = pos; lastMoveMs_ = now; }
        else if (p_.prof && elapsed(now, lastMoveMs_) >= p_.prof->stallTimeoutMs) { finish(StepOutcome::failed(JobResult::Stalled, Reason::Stall, pos)); return pending_; }

        Ticks remaining = (p_.target - pos) * dir_;
        if (remaining < 0) remaining = 0;

        // slow progress, measured per 1000 ms window during cruise
        if (phase_ == Phase::Cruise && p_.prof && elapsed(now, slowWindowStart_) >= 1000) {
            Ticks rate = absl(pos - slowWindowTicks_);       // ticks per second
            slowWindowStart_ = now; slowWindowTicks_ = pos;
            if (rate < static_cast<Ticks>(p_.prof->slowProgressMinTicksPerSec) && remaining > kSlowRemainingMin) {
                if (!reboosted_) { reboosted_ = true; enter(Phase::Breakaway, pos, now); phaseChanged_ = true; slowAccumMs_ += 1000; }
                else {
                    slowAccumMs_ += 1000;
                    if (slowAccumMs_ >= p_.prof->slowProgressMaxMs) { finish(StepOutcome::failed(JobResult::Stalled, Reason::SlowProgress, pos)); return pending_; }
                }
            }
        }

        Throttle power = static_cast<Throttle>(p_.speed * dir_);
        switch (phase_) {
            case Phase::Breakaway: {
                out = driveCmd(power);
                Ticks moved = (pos - phaseStartTicks_) * dir_;
                if (moved >= kBreakawayTicks) {
                    Ticks cap = p_.prof ? p_.prof->breakawayRampTicksCap : 80;
                    rampTicks_ = clampv<Ticks>(distance_ / 3, 20, cap);
                    enter(Phase::Ramp, pos, now); phaseChanged_ = true;
                } else if (p_.prof && elapsed(now, phaseMs_) >= p_.prof->breakawayMaxMs) {
                    finish(StepOutcome::failed(JobResult::Stalled, Reason::Breakaway, pos)); return pending_;
                }
                break;
            }
            case Phase::Ramp: {
                Ticks moved = (pos - phaseStartTicks_) * dir_;
                if (moved >= rampTicks_) { enter(Phase::Cruise, pos, now); phaseChanged_ = true; out = pulse_.tickOn() ? driveCmd(cruisePower(remaining, power)) : brakeCmd(); break; }
                int band = (moved * 100) / (rampTicks_ ? rampTicks_ : 1);
                if (band < 75) out = driveCmd(power);
                else if (band < 90) { PulsePattern q; q.onTicks = pulse_.onTicks; q.offTicks = 1; q.phase = pulse_.phase; out = q.tickOn() ? driveCmd(power) : brakeCmd(); pulse_.phase = q.phase; }
                else out = pulse_.tickOn() ? driveCmd(power) : brakeCmd();
                break;
            }
            case Phase::Cruise:
                out = pulse_.tickOn() ? driveCmd(cruisePower(remaining, power)) : brakeCmd();
                break;
            default: break;
        }
        return StepOutcome::running();
    }

    Phase phase() const { return phase_; }
    bool takePhaseChanged() { bool c = phaseChanged_; phaseChanged_ = false; return c; }
    int  dir() const { return dir_; }
    Ticks target() const { return p_.target; }
    Ticks distance() const { return distance_; }
    Ticks startTicks() const { return start_; }
    Ticks floorTicks() const { return p_.floorTicks; }
    bool  stoppedByFloor() const { return stoppedByFloor_; }   // the floor, not the target, ended it

  private:
    // The speed in ticks per 16 ms. While the mast is gathering speed the 16 ms average trails it: a
    // short step brakes during its breakaway burst (0.62 ticks/ms averaged against 1.0 really, on
    // the simulated 19:1). So when the last 8 ms clearly differ from the 8 before them (2 ticks or
    // more; steady running, pulses included, differs by a tick at most), the speed is carried
    // forward: the last 8 ms plus a quarter of the change. A quarter, not the straight-line half,
    // because the speed levels off as it nears full (a sweep of steps 10-400 ticks on the simulator:
    // mean error 2.5 ticks against 3.3 for a half and 6.5 for none). `high` keeps the larger of the
    // two figures (for the floor: brake earlier, never later).
    Ticks speed16(Ticks pos, Ticks mid, Ticks back, bool high) const {
        Ticks moved = (pos - back) * dir_;
        Ticks late = (pos - mid) * dir_, early = (mid - back) * dir_;           // the two 8 ms halves
        if (late - early >= 2 || late - early <= -2) {
            Ticks now = (5 * late - early + 1) / 2;                             // 2 x (late + (late - early) / 4)
            if (now < 0) now = 0;
            if (!high || now > moved) moved = now;
        }
        return moved;
    }
    // Ticks the mast will run on if the brake goes on now (rounded up).
    Ticks aheadTicks(Ticks pos, Ticks mid, Ticks back) const {
        Ticks moved = speed16(pos, mid, back, false);
        if (moved <= 0) return 0;
        int ms = p_.brake.msAt(moved * 1000 / kVelWindow);
        return (moved * ms + kVelWindow - 1) / kVelWindow;
    }
    // The same against the floor, on the safe side: the higher speed, the longer time, the margin.
    Ticks floorAheadTicks(Ticks pos, Ticks mid, Ticks back) const {
        Ticks moved = speed16(pos, mid, back, true), ahead = 0;
        if (moved > 0) {
            int ms = p_.brake.n ? p_.brake.msAt(moved * 1000 / kVelWindow) : kUncalibratedBrakeMs;
            if (ms < p_.brake.medianMs) ms = p_.brake.medianMs;
            ahead = (moved * ms + kVelWindow - 1) / kVelWindow;
        }
        return ahead + ahead / 4 + 1;
    }
    void enter(Phase ph, Ticks pos, Ms now) { phase_ = ph; phaseMs_ = now; phaseStartTicks_ = pos; }
    void beginBrake(StepOutcome pending, Ms now) { pending_ = pending; brakeUntil_ = now + kBrakeMs; phase_ = Phase::Braking; phaseChanged_ = true; }
    void finish(StepOutcome o) { pending_ = o; phase_ = Phase::Finished; phaseChanged_ = true; }
    Throttle cruisePower(Ticks remaining, Throttle full) const {
        // Full-travel seek past its count target with no switch yet: the stored travel is
        // wrong (bench 2026-09-19: 450 configured on a ~1150-tick lifter). Keep going at the
        // minimum reliable power rather than the soft-approach floor, which can stall.
        if (p_.fullTravel && remaining <= 0) {
            Throttle m = p_.minPower > 0 ? p_.minPower : clampThrottle(p_.approachPower);
            Throttle mag = static_cast<Throttle>(full < 0 ? -full : full);
            if (m > mag) m = mag;
            return static_cast<Throttle>(dir_ > 0 ? m : -m);
        }
        Ticks zone = 0;
        if (dir_ > 0) { if (p_.prof && p_.prof->softApproachEnabled) zone = clampv<Ticks>(distance_ * p_.prof->softApproachPct / 100, 30, 1 << 30); }
        else zone = clampv<Ticks>(distance_ / 5, 30, 1 << 30);
        if (zone <= 0 || remaining >= zone) return full;
        Throttle approach = clampThrottle(p_.approachPower);
        Throttle mag = static_cast<Throttle>(full < 0 ? -full : full);
        if (approach > mag) approach = mag;
        Throttle m = static_cast<Throttle>(approach + (mag - approach) * remaining / zone);
        return static_cast<Throttle>(dir_ > 0 ? m : -m);
    }

    SeekParams p_;
    int dir_ = 1;
    Ticks start_ = 0, distance_ = 0, phaseStartTicks_ = 0, lastTicks_ = 0, slowWindowTicks_ = 0, rampTicks_ = 0;
    Ms startMs_ = 0, phaseMs_ = 0, lastMoveMs_ = 0, slowWindowStart_ = 0, slowAccumMs_ = 0, brakeUntil_ = 0;
    bool reboosted_ = false, phaseChanged_ = false, stoppedByFloor_ = false;
    Ticks hist_[kVelWindow] = {};
    int histIdx_ = 0;
    PulsePattern pulse_;
    Phase phase_ = Phase::Idle;
    StepOutcome pending_;
};

// ---------------------------------------------------------------- creep / jog

struct CreepParams {
    int      dir = 1;             // +1 up, -1 down
    Throttle power = 300;         // continuous drive
    Ticks    floorTicks = -2147483647; // stop here when descending and not homed (policy)
    bool     jog = false;         // jog: lease-based, no stall exit; else wizard creep
    Ms       stallMs = 1500;      // wizard only
    Ms       maxMs = 30000;       // wizard only
    Ticks    stopAfter = 0;       // step ("#PNUDGE"): stop after this much travel; 0 = run to a limit (or the lease)
    int      brakeLeadMs = 0;     // start braking this much travel-time early (brakeLeadMs()); 0 = on the mark
    int      floorLeadMs = 0;     // the same against the policy floor; never 0 there (see the supervisor)
};

// A step and the policy floor are targets the mast must come to REST on, not merely reach: a
// short brake takes a distance of about speed x a fixed time to stop the mast (bench 2026-10-03:
// a 15 % creep stopping "at" 1100 came to rest at 1088). So both start braking `ahead()` ticks
// early: the speed over the last kVelWindow ms times the calibrated brake lead. The floor adds a
// quarter and a tick more, because there the error must land on the safe side.
class LifterCreep {
  public:
    static constexpr Ms kBrakeMs = 60;
    static constexpr int kVelWindow = 16;     // ms of position history for the speed estimate
    void start(const CreepParams& p, Ticks current, Ms now) {
        p_ = p; startMs_ = now; start_ = current; lastTicks_ = current; lastMoveMs_ = now; braking_ = false; finished_ = false; primed_ = false;
        for (Ticks& h : hist_) h = current;
        histIdx_ = 0;
    }
    StepOutcome step(const TickInputs& in, BridgeCommand& out) {
        Ticks pos = in.lifterTicks; Ms now = in.nowMs;
        StepOutcome o; out = brakeCmd();
        if (finished_) return pending_;
        if (universalExit(in, in.lifterDiag, o)) { pending_ = o; finished_ = true; return pending_; }
        if (braking_) { if (static_cast<int32_t>(now - brakeUntil_) >= 0) { finished_ = true; return pending_; } return StepOutcome::running(); }
        Ticks back = hist_[histIdx_]; hist_[histIdx_] = pos; histIdx_ = (histIdx_ + 1) % kVelWindow;   // where it was kVelWindow ms ago
        Ticks ahead = aheadTicks(pos, back, p_.brakeLeadMs);
        if ((p_.dir > 0 && in.topLimit) || (p_.dir < 0 && in.bottomLimit)) return beginBrake(StepOutcome::done(JobResult::DoneLimit, Reason::LimitReached, pos), now);
        // The switch at the end we are moving AWAY from can only make if the mast is going the
        // other way: a reversed mechanism (v3's "inverted") or swapped limit wires. The encoder
        // rides the motor shaft, so it cannot tell; the switches can. Stop at once instead of
        // driving on into the mechanical stop.
        bool away = p_.dir > 0 ? in.bottomLimit : in.topLimit;
        if (primed_ && away && !awayWas_) return beginBrake(StepOutcome::failed(JobResult::Failed, Reason::WrongLimit, pos), now);
        awayWas_ = away; primed_ = true;
        Ticks floorLead = aheadTicks(pos, back, p_.floorLeadMs > p_.brakeLeadMs ? p_.floorLeadMs : p_.brakeLeadMs);
        Ticks floorAhead = floorLead > 0 ? floorLead + floorLead / 4 + 1 : 0;
        if (p_.dir < 0 && pos - floorAhead <= p_.floorTicks) return beginBrake(StepOutcome::done(JobResult::Clamped, Reason::RotaryNotHome, pos), now);
        if (p_.stopAfter > 0 && (pos - start_) * p_.dir + ahead >= p_.stopAfter) return beginBrake(StepOutcome::done(JobResult::DoneTarget, Reason::TargetReached, pos), now);
        if (p_.jog && !in.jogLeaseLive) return beginBrake(StepOutcome::done(JobResult::Done, Reason::LeaseExpired, pos), now);
        // A stall ends every creep, a held jog included (bench 2026-10-03: a jog that met the
        // lifter frame kept pushing for as long as the button was held).
        if (pos != lastTicks_) { lastTicks_ = pos; lastMoveMs_ = now; }
        else if (elapsed(now, lastMoveMs_) >= p_.stallMs) { pending_ = StepOutcome::failed(JobResult::Stalled, Reason::Stall, pos); finished_ = true; return pending_; }
        if (!p_.jog && elapsed(now, startMs_) >= p_.maxMs) return beginBrake(StepOutcome::failed(JobResult::Failed, Reason::Timeout, pos), now);
        out = driveCmd(static_cast<Throttle>(p_.power * p_.dir));
        return StepOutcome::running();
    }
    int dir() const { return p_.dir; }
    bool jog() const { return p_.jog; }
    Ticks stopAfter() const { return p_.stopAfter; }
    Ticks startTicks() const { return start_; }
    Ticks floorTicks() const { return p_.floorTicks; }
  private:
    StepOutcome beginBrake(StepOutcome pending, Ms now) { pending_ = pending; brakeUntil_ = now + kBrakeMs; braking_ = true; return StepOutcome::running(); }
    // ticks the mast will run on if the brake goes on now (rounded up; 0 without a lead)
    Ticks aheadTicks(Ticks pos, Ticks back, int leadMs) const {
        Ticks moved = (pos - back) * p_.dir;
        if (moved <= 0 || leadMs <= 0) return 0;
        return (moved * leadMs + kVelWindow - 1) / kVelWindow;
    }
    Ticks hist_[kVelWindow] = {};
    int histIdx_ = 0;
    CreepParams p_;
    Ms startMs_ = 0, lastMoveMs_ = 0, brakeUntil_ = 0;
    Ticks start_ = 0, lastTicks_ = 0;
    bool braking_ = false, finished_ = false, primed_ = false, awayWas_ = false;
    StepOutcome pending_;
};

} // namespace uppity

// ---------------------------------------------------------------- calibration
// (appended; see docs/REWRITE-PLAN.md Appendix F.3 "LifterCalibrate")
namespace uppity {

// How early a creep starts braking, as a time. The calibration stored, for each 5 % slot, the
// cruise speed (ticks/s) and how far the mast ran on after the brake (ticks); their ratio is close
// to constant on a lead-screw lifter, because a short brake's drag grows with the speed. Bench
// 2026-10-03/04 on Todd's 19:1, going down: 12 ticks from a 15 % creep and 53-60 from full speed
// are both about 40 ms of travel. Median over the measured slots, at most 150 ms; 0 without a
// calibration (creeps then brake on the mark, as before).
inline int brakeLeadMs(const uint16_t over[kSlots], const uint16_t vel[kSlots]) {
    int v[kSlots]; int n = 0;
    for (int i = 0; i < kSlots; ++i)
        if (vel[i] > 0 && over[i] > 0) v[n++] = static_cast<int>((static_cast<int32_t>(over[i]) * 1000 + vel[i] / 2) / vel[i]);
    if (n == 0) return 0;
    for (int i = 1; i < n; ++i) { int x = v[i], j = i - 1; while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; --j; } v[j + 1] = x; }
    int m = (n % 2) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2] + 1) / 2;
    return m > 150 ? 150 : m;
}

struct CalibrateParams {
    const LifterMotorProfile* prof = nullptr;
    Throttle sweepCap = 1000;        // highest slot to try
    Throttle startPower = 650;       // profile default minimum power
    Ms cooldownMs = 2500;
    Ticks travelHint = 0;            // stored travel, for the mid-travel target before P1 finishes
};

struct CalibrateResult {
    Ticks travelUp[2] = {0, 0}, travelDown[2] = {0, 0};
    Ticks travel = 0;
    bool  travelOk = false;
    float travelSpreadPct = 0;
    Throttle minUp = 0, minDown = 0;
    bool  slotValidUp[kSlots] = {}, slotValidDown[kSlots] = {};
    uint16_t velUp[kSlots] = {}, velDown[kSlots] = {};        // ticks per second at that slot
    uint16_t overUp[kSlots] = {}, overDown[kSlots] = {};      // coast ticks after the brake command
    int slotsMeasured = 0;
    Ticks bottomMake = 0;           // raw count where the bottom switch last closed on the way down
    bool  bottomMakeValid = false;  // (the supervisor re-references there: that point is 0)
    Ticks bottomRest = 0;           // deepest the mast settled below that closing point (>= 0): at
                                    // power-up on the bottom switch the count starts at minus this
};

class LifterCalibrate {
  public:
    enum class Phase : uint8_t { Idle, P1Travel, P2MinUp, P2MinDown, P3Up, P3Down, Finishing, Finished };
    static constexpr Ms kOvershootWindowMs = 200;
    static constexpr Ticks kBreakawayTicks = 10;

    void start(const CalibrateParams& p, Ticks current, Ms now) {
        p_ = p; res_ = CalibrateResult{};
        phase_ = Phase::P1Travel; sub_ = 0; attempt_ = 0; idx_ = 0;
        cooldownUntil_ = now; base_ = 0; edgeTicks_ = current; phaseChanged_ = true;
        creepUp_ = static_cast<Throttle>(p.startPower + 50 > 1000 ? 1000 : p.startPower + 50);
        slotPower_ = 0; slotIdx_ = 0; measuring_ = false; braking_ = false; bursting_ = false; pendingCreep_ = 0;
        queueCreep(-1);                                  // first: reach the bottom
        (void)current;
    }

    StepOutcome step(const TickInputs& in, BridgeCommand& out) {
        Ticks pos = in.lifterTicks; Ms now = in.nowMs; StepOutcome o; out = brakeCmd();
        if (phase_ == Phase::Finished) return pending_;
        if (universalExit(in, in.lifterDiag, o)) { finish(o); return pending_; }
        noteBottomRest(in);
        if (static_cast<int32_t>(now - cooldownUntil_) < 0) return StepOutcome::running();   // rest between attempts
        if (pendingCreep_ != 0) { beginCreep(pendingCreep_, pos, now); pendingCreep_ = 0; }   // creeps start after the rest, not before
        switch (phase_) {
            case Phase::P1Travel:   return p1(in, out);
            case Phase::P2MinUp:    return p2(in, out, +1);
            case Phase::P2MinDown:  return p2(in, out, -1);
            case Phase::P3Up:       return p3(in, out, +1);
            case Phase::P3Down:     return p3(in, out, -1);
            case Phase::Finishing:  return finishing(in, out);
            default: (void)pos; return StepOutcome::running();
        }
    }

    Phase phase() const { return phase_; }
    bool takePhaseChanged() { bool c = phaseChanged_; phaseChanged_ = false; return c; }
    const CalibrateResult& result() const { return res_; }
    // Progress for the web page: P1 = limit-to-limit passes done (0..4), P2 = the power being
    // tried (permille), P3 = the 5 % slot being measured (0..20); 0 otherwise.
    int16_t progress() const {
        switch (phase_) {
            case Phase::P1Travel: return static_cast<int16_t>(sub_);
            case Phase::P2MinUp: case Phase::P2MinDown: return static_cast<int16_t>(tryPower_);
            case Phase::P3Up: case Phase::P3Down: return static_cast<int16_t>(slotIdx_);
            default: return 0;
        }
    }

  private:
    // ---- creep helper (continuous drive to a limit) ----
    void queueCreep(int dir) { pendingCreep_ = dir; }
    void beginCreep(int dir, Ticks current, Ms now) {
        CreepParams c; c.dir = dir; c.power = creepUp_; c.jog = false; c.stallMs = p_.prof ? p_.prof->stallTimeoutMs : 2000; c.maxMs = 60000;
        creep_.start(c, current, now); creeping_ = true;
    }
    // returns true when the creep finished on its limit; fails the job on anything else
    bool creepDone(const TickInputs& in, BridgeCommand& out, StepOutcome& fail) {
        StepOutcome o = creep_.step(in, out);
        if (o.result == StepResult::Running) return false;
        creeping_ = false;
        if (o.jobResult != JobResult::DoneLimit) { fail = StepOutcome::failed(JobResult::Failed, o.reason == Reason::None ? Reason::Stall : o.reason, in.lifterTicks); return true; }
        // The creep reports where the switch closed (detail); a creep that started on its switch
        // closed nothing, and its "make" is just where the mast was resting.
        lastMake_ = o.detail; lastMakeReal_ = absl(o.detail - creep_.startTicks()) >= 5;
        if (creep_.dir() < 0 && lastMakeReal_) { res_.bottomMake = o.detail; res_.bottomMakeValid = true; noteBottomRest(in); }
        fail.result = StepResult::Running;
        return true;
    }
    void enter(Phase ph, Ms now) { phase_ = ph; sub_ = 0; attempt_ = 0; idx_ = 0; phaseChanged_ = true; (void)now; }
    // How far below the bottom switch's closing point the mast settles: the deepest point seen with
    // the switch made, measured from the last closing the mast drove into. Power-up on that switch
    // starts the count there (2026-10-04: it started at 0 at the resting spot, ~33 ticks below the
    // closing point, so the height read ~33 high until the mast passed a switch).
    void noteBottomRest(const TickInputs& in) {
        if (!res_.bottomMakeValid || !in.bottomLimit) return;
        Ticks d = res_.bottomMake - in.lifterTicks;
        if (d > res_.bottomRest) res_.bottomRest = d;
    }
    void finish(StepOutcome o) { pending_ = o; phase_ = Phase::Finished; phaseChanged_ = true; }
    void rest(Ms now) { cooldownUntil_ = now + p_.cooldownMs; }

    // P1: bottom -> top -> bottom -> top -> bottom. Travel is counted between the points where the
    // switches CLOSE, because that is how the frame is referenced (bottom closing = 0, top closing
    // = travel): measured between the resting spots it included how far the mast runs on past each
    // switch while braking (2026-10-04 on Todd's 19:1: 1192 against about 1138 switch to switch),
    // and the count jumped by the difference at every switch. The rests stay the references for P2/P3.
    StepOutcome p1(const TickInputs& in, BridgeCommand& out) {
        if (!creeping_) return StepOutcome::running();     // a queued creep starts on the next tick
        StepOutcome fail;
        if (!creepDone(in, out, fail)) return StepOutcome::running();
        if (fail.result != StepResult::Running) { finish(fail); return pending_; }
        Ticks pos = in.lifterTicks;
        switch (sub_) {
            case 0: base_ = pos; makeLo_ = lastMake_; firstLoReal_ = lastMakeReal_; ++sub_; queueCreep(+1); break;   // at bottom: reference
            case 1: res_.travelUp[0] = lastMake_ - makeLo_; makeHi_ = lastMake_; edgeTicks_ = pos; ++sub_; rest(in.nowMs); queueCreep(-1); break;
            case 2: res_.travelDown[0] = makeHi_ - lastMake_; makeLo_ = lastMake_; base_ = pos; ++sub_; rest(in.nowMs); queueCreep(+1); break;
            case 3: res_.travelUp[1] = lastMake_ - makeLo_; makeHi_ = lastMake_; edgeTicks_ = pos; ++sub_; rest(in.nowMs); queueCreep(-1); break;
            case 4: {
                res_.travelDown[1] = makeHi_ - lastMake_; base_ = pos;
                // the first up-run counts only if the first creep really closed the bottom switch
                // (one that started on it measured from the resting spot)
                Ticks v[4] = { res_.travelUp[1], res_.travelDown[0], res_.travelDown[1], res_.travelUp[0] };
                int n = firstLoReal_ ? 4 : 3;
                Ticks lo = v[0], hi = v[0], sum = 0;
                for (int i = 0; i < n; ++i) { sum += v[i]; if (v[i] < lo) lo = v[i]; if (v[i] > hi) hi = v[i]; }
                Ticks mean = sum / n;
                res_.travelSpreadPct = mean > 0 ? (hi - lo) * 100.f / mean : 100.f;
                if (mean < 20 || res_.travelSpreadPct > 5.f) { finish(StepOutcome::failed(JobResult::Failed, Reason::NotRepeatable, static_cast<int32_t>(res_.travelSpreadPct * 10))); return pending_; }
                res_.travel = mean; res_.travelOk = true;
                rest(in.nowMs); enter(Phase::P2MinUp, in.nowMs); tryPower_ = p_.startPower; lastGood_ = 0; probing_ = false;
                break;
            }
        }
        return StepOutcome::running();
    }

    // P2: from a limit, step the power down 5 % at a time while a continuous burst still moves
    // 10 ticks within the profile's breakaway time; the floor is the lowest success plus 5 %.
    StepOutcome p2(const TickInputs& in, BridgeCommand& out, int dir) {
        Ticks pos = in.lifterTicks; Ms now = in.nowMs;
        if (creeping_) {                                     // returning to the reference limit
            StepOutcome fail; if (!creepDone(in, out, fail)) return StepOutcome::running();
            if (fail.result != StepResult::Running) { finish(fail); return pending_; }
            rest(now); return StepOutcome::running();
        }
        if (!probing_) {
            if (tryPower_ < 100 || (lastGood_ != 0 && tryPower_ < lastGood_ - 300)) { p2Finish(dir, in); return StepOutcome::running(); }
            probing_ = true; probeStart_ = now; probeTicks_ = pos;
        }
        Ticks moved = (pos - probeTicks_) * dir;
        Ms maxMs = p_.prof ? p_.prof->breakawayMaxMs : 500;
        if (moved >= kBreakawayTicks) {
            lastGood_ = tryPower_; probing_ = false; tryPower_ = static_cast<Throttle>(tryPower_ - 50);
            queueCreep(-dir);                      // back to the limit we started from
            return StepOutcome::running();
        }
        if (elapsed(now, probeStart_) >= maxMs || ((dir > 0 && in.topLimit) || (dir < 0 && in.bottomLimit))) {
            probing_ = false;
            if ((dir > 0 && in.topLimit) || (dir < 0 && in.bottomLimit)) { lastGood_ = tryPower_; }
            p2Finish(dir, in);
            return StepOutcome::running();
        }
        out = driveCmd(static_cast<Throttle>(tryPower_ * dir));
        return StepOutcome::running();
    }
    void p2Finish(int dir, const TickInputs& in) {
        Throttle floor = lastGood_ != 0 ? static_cast<Throttle>(lastGood_ + 50) : static_cast<Throttle>(p_.startPower + 50);
        if (floor > 1000) floor = 1000;
        if (dir > 0) {
            res_.minUp = floor; rest(in.nowMs);
            queueCreep(+1);        // go to the top for the down probes
            enter(Phase::P2MinDown, in.nowMs); tryPower_ = p_.startPower; lastGood_ = 0; probing_ = false;
        } else {
            res_.minDown = floor; rest(in.nowMs);
            queueCreep(-1);        // back to the bottom for the up slots
            enter(Phase::P3Up, in.nowMs); slotIdx_ = 0; measuring_ = false; braking_ = false;
        }
    }

    // P3: for each 5 % slot from the floor to the cap, pulsed cruise over the middle of the
    // travel, measure ticks/s and the coast after braking, then creep back.
    StepOutcome p3(const TickInputs& in, BridgeCommand& out, int dir) {
        Ticks pos = in.lifterTicks; Ms now = in.nowMs;
        if (creeping_) {
            StepOutcome fail; if (!creepDone(in, out, fail)) return StepOutcome::running();
            if (fail.result != StepResult::Running) { finish(fail); return pending_; }
            if (dir > 0) base_ = pos; else edgeTicks_ = pos;
            rest(now); return StepOutcome::running();
        }
        Throttle floor = dir > 0 ? res_.minUp : res_.minDown;
        int firstSlot = (floor + 49) / 50, capSlot = p_.sweepCap / 50;
        if (!measuring_ && !braking_) {
            if (slotIdx_ < firstSlot) slotIdx_ = firstSlot;
            if (slotIdx_ > capSlot || slotIdx_ >= kSlots) {
                if (dir > 0) { enter(Phase::P3Down, now); slotIdx_ = 0; queueCreep(+1); }
                else { enter(Phase::Finishing, now); queueCreep(-1); }
                return StepOutcome::running();
            }
            slotPower_ = static_cast<Throttle>(slotIdx_ * 50);
            measuring_ = true; measureStart_ = 0; measureTicks_ = 0; pulse_.set(p_.prof ? p_.prof->pulseOnMs : 3, p_.prof ? p_.prof->pulseOffMs : 1);
            cruiseStart_ = pos; cruiseStartMs_ = now; lastTicks_ = pos; lastMoveMs_ = now;
            bursting_ = true;                                // breakaway burst first, as a real seek does
        }
        Ticks travel = res_.travel > 0 ? res_.travel : 450;
        Ticks from = dir > 0 ? base_ : edgeTicks_;
        Ticks progress = (pos - from) * dir;
        if (measuring_) {
            if (pos != lastTicks_) { lastTicks_ = pos; lastMoveMs_ = now; }
            else if (p_.prof && elapsed(now, lastMoveMs_) >= p_.prof->stallTimeoutMs) {   // this slot cannot sustain motion
                measuring_ = false; ++slotIdx_; rest(now); queueCreep(-dir); return StepOutcome::running();
            }
            if (progress >= travel / 10 && measureStart_ == 0) { measureStart_ = now; measureTicks_ = pos; }
            bool endOfRun = progress >= travel / 2 || (dir > 0 ? in.topLimit : in.bottomLimit);
            if (endOfRun) {
                Ms dt = measureStart_ ? elapsed(now, measureStart_) : elapsed(now, cruiseStartMs_);
                Ticks dticks = measureStart_ ? absl(pos - measureTicks_) : absl(pos - cruiseStart_);
                uint16_t vel = dt > 0 ? static_cast<uint16_t>(clampv<int32_t>(dticks * 1000 / static_cast<int32_t>(dt), 0, 65535)) : 0;
                if (dir > 0) { res_.velUp[slotIdx_] = vel; res_.slotValidUp[slotIdx_] = true; } else { res_.velDown[slotIdx_] = vel; res_.slotValidDown[slotIdx_] = true; }
                measuring_ = false; braking_ = true; brakeStart_ = now; brakeTicks_ = pos; ++res_.slotsMeasured;
                return StepOutcome::running();
            }
            if (bursting_) {
                Ms maxMs = p_.prof ? p_.prof->breakawayMaxMs : 500;
                if (absl(pos - cruiseStart_) >= kBreakawayTicks || elapsed(now, cruiseStartMs_) >= maxMs) bursting_ = false;
                out = driveCmd(static_cast<Throttle>(slotPower_ * dir));
                return StepOutcome::running();
            }
            out = pulse_.tickOn() ? driveCmd(static_cast<Throttle>(slotPower_ * dir)) : brakeCmd();
            return StepOutcome::running();
        }
        // braking: measure the coast
        if (elapsed(now, brakeStart_) >= kOvershootWindowMs) {
            uint16_t over = static_cast<uint16_t>(clampv<int32_t>(absl(pos - brakeTicks_), 0, 65535));
            if (dir > 0) res_.overUp[slotIdx_] = over; else res_.overDown[slotIdx_] = over;
            braking_ = false; ++slotIdx_; rest(now);
            queueCreep(-dir);
        }
        return StepOutcome::running();
    }

    StepOutcome finishing(const TickInputs& in, BridgeCommand& out) {
        if (creeping_) { StepOutcome fail; if (!creepDone(in, out, fail)) return StepOutcome::running(); if (fail.result != StepResult::Running) { finish(fail); return pending_; } }
        finish(StepOutcome::done(JobResult::Done, Reason::None, res_.travel));
        return pending_;
    }

    CalibrateParams p_;
    CalibrateResult res_;
    LifterCreep creep_;
    PulsePattern pulse_;
    Phase phase_ = Phase::Idle;
    int sub_ = 0, attempt_ = 0, idx_ = 0, slotIdx_ = 0;
    Ticks base_ = 0, edgeTicks_ = 0, probeTicks_ = 0, cruiseStart_ = 0, measureTicks_ = 0, brakeTicks_ = 0, lastTicks_ = 0;
    Ticks lastMake_ = 0, makeLo_ = 0, makeHi_ = 0;     // P1: where the switches closed (raw counts)
    bool lastMakeReal_ = false, firstLoReal_ = false;
    Ms cooldownUntil_ = 0, probeStart_ = 0, cruiseStartMs_ = 0, measureStart_ = 0, brakeStart_ = 0, lastMoveMs_ = 0;
    Throttle creepUp_ = 700, tryPower_ = 0, lastGood_ = 0, slotPower_ = 0;
    bool creeping_ = false, probing_ = false, measuring_ = false, braking_ = false, bursting_ = false, phaseChanged_ = false;
    int pendingCreep_ = 0;
    StepOutcome pending_;
};

} // namespace uppity
