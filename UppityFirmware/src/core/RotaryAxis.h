#pragma once
#include "Units.h"
#include "Profiles.h"
#include "Knowledge.h"
#include "CircularMath.h"
#include "MotionTypes.h"
// Rotary state machines (docs/REWRITE-PLAN.md Appendix F.3, F.4). Pure.
//
// Conventions: "tick direction" +1 means the encoder count increases. The characterisation's
// driveSign maps that onto motor polarity (v3 wiring: negative throttle increases the count).
// Spin and jog take a raw signed throttle (":PR+" = positive motor polarity, as v3).
namespace uppity {

// Slew-limited throttle. Below the floor the motor does not move, so the ramp starts at the floor.
struct Slew {
    Throttle cur = 0;
    Ms last = 0;
    void reset(Ms now) { cur = 0; last = now; }
    Throttle step(Throttle target, Ms now, const RotaryCharacterisation& rc, Throttle floor) {
        if (target == 0) { if (elapsed(now, last) >= rc.rampLatencyMs) { last = now; int d = -cur; int m = clampv<int>(absi(d), 0, rc.slewDecelPermille); cur = static_cast<Throttle>(cur + sgn(d) * m); } return cur; }
        int tsign = target > 0 ? 1 : -1;
        if (cur == 0 || (cur > 0) != (target > 0)) { cur = static_cast<Throttle>(tsign * floor); last = now; if (absi(target) < floor) cur = static_cast<Throttle>(tsign * floor); return cur; }
        if (elapsed(now, last) < rc.rampLatencyMs) return cur;
        last = now;
        int d = target - cur;
        bool accel = absi(target) > absi(cur);
        int m = clampv<int>(absi(d), 0, accel ? rc.slewAccelPermille : rc.slewDecelPermille);
        cur = static_cast<Throttle>(cur + sgn(d) * m);
        if (absi(cur) < floor) cur = static_cast<Throttle>(tsign * floor);
        return cur;
    }
};

// ---------------------------------------------------------------- move-to-ticks engine

struct RotaryMoveParams {
    Ticks    target = 0;          // absolute ticks in the current frame, never normalised
    Throttle speed = 0;           // permille, 0 = floor
    Throttle maxSpeed = 0;        // permille, 0 = speed
    Throttle floor = 400;
    Ticks    fudge = 16;          // arrival window (5 deg)
    Ticks    stopDist = 8;        // brake this far before the target
    Ticks    halfRev = 590;       // for speed scaling by distance
    Ms       stallMs = 500;
    bool     allowFlip = true;    // one reversal to correct an overshoot
    Ticks    stepBelow = 0;       // a move no longer than this is walked one tick at a time (brake, pause, next tick):
                                  // a continuous move cannot land within its own coast distance
    const RotaryCharacterisation* rc = nullptr;
};

class RotaryMove {
  public:
    enum class Phase : uint8_t { Idle, Moving, Braking, Finished };
    static constexpr Ms kBrakeMs = 50;
    static constexpr Ms kStepPauseMs = 40;
    static constexpr int kMaxSteps = 80;
    void start(const RotaryMoveParams& p, Ticks current, Ms now) {
        p_ = p; if (p_.speed <= 0) p_.speed = p_.floor; if (p_.maxSpeed < p_.speed) p_.maxSpeed = p_.speed;
        dir_ = p.target >= current ? 1 : -1; flips_ = 0; phase_ = Phase::Moving; slew_.reset(now);
        lastTicks_ = current; lastMoveMs_ = now; brakeUntil_ = 0; stepping_ = false; steps_ = 0;
        Ticks dist = absl(p.target - current);
        if (dist <= p_.fudge) { phase_ = Phase::Finished; pending_ = StepOutcome::done(JobResult::DoneTarget, Reason::AlreadyThere, 0); return; }
        if (p_.stepBelow > 0 && dist <= p_.stepBelow) { stepping_ = true; finalTarget_ = p.target; p_.fudge = 0; p_.stopDist = 0; p_.allowFlip = false; beginStep(current, now); }
    }
    StepOutcome step(const TickInputs& in, BridgeCommand& out) {
        Ticks pos = in.rotaryTicks; Ms now = in.nowMs; StepOutcome o; out = brakeCmd();
        if (phase_ == Phase::Finished) return pending_;
        if (universalExit(in, in.rotaryDiag, o)) { finish(o); return pending_; }
        if (phase_ == Phase::Braking) {
            if (static_cast<int32_t>(now - brakeUntil_) < 0) return StepOutcome::running();
            if (stepping_) {
                Ticks rem = (finalTarget_ - pos) * dir_;
                if (rem <= 0 || steps_ >= kMaxSteps) { finish(StepOutcome::done(JobResult::DoneTarget, Reason::TargetReached, finalTarget_ - pos)); return pending_; }
                beginStep(pos, now); return StepOutcome::running();
            }
            Ticks err = p_.target - pos;
            if (absl(err) <= p_.fudge) { finish(StepOutcome::done(JobResult::DoneTarget, Reason::TargetReached, err)); return pending_; }
            if (p_.allowFlip && flips_ < 1) { ++flips_; dir_ = err > 0 ? 1 : -1; phase_ = Phase::Moving; slew_.reset(now); lastTicks_ = pos; lastMoveMs_ = now; return StepOutcome::running(); }
            finish(StepOutcome::done(JobResult::Done, Reason::Overshoot, err)); return pending_;
        }
        Ticks remaining = (p_.target - pos) * dir_;
        if (remaining <= p_.stopDist) { phase_ = Phase::Braking; brakeUntil_ = now + kBrakeMs + (stepping_ ? kStepPauseMs : 0); return StepOutcome::running(); }
        if (pos != lastTicks_) { lastTicks_ = pos; lastMoveMs_ = now; }
        else if (elapsed(now, lastMoveMs_) >= p_.stallMs) { finish(StepOutcome::failed(JobResult::Stalled, Reason::Stall, pos)); return pending_; }
        Throttle cmd = p_.speed;
        if (p_.maxSpeed > p_.speed && p_.halfRev > 0) { Ticks r = remaining < p_.halfRev ? remaining : p_.halfRev; cmd = static_cast<Throttle>(p_.speed + (p_.maxSpeed - p_.speed) * r / p_.halfRev); }
        int sign = (p_.rc ? p_.rc->driveSign : 1) * dir_;
        Throttle t = slew_.step(static_cast<Throttle>(sign * cmd), now, *p_.rc, p_.floor);
        out = driveCmd(t);
        return StepOutcome::running();
    }
    Phase phase() const { return phase_; }
    int flips() const { return flips_; }
    bool stepping() const { return stepping_; }
  private:
    void finish(StepOutcome o) { pending_ = o; phase_ = Phase::Finished; }
    void beginStep(Ticks pos, Ms now) { dir_ = finalTarget_ > pos ? 1 : -1; p_.target = pos + dir_; ++steps_; phase_ = Phase::Moving; slew_.reset(now); lastTicks_ = pos; lastMoveMs_ = now; }
    RotaryMoveParams p_;
    Ticks finalTarget_ = 0;
    bool stepping_ = false;
    int steps_ = 0;
    int dir_ = 1, flips_ = 0;
    Phase phase_ = Phase::Idle;
    Slew slew_;
    Ticks lastTicks_ = 0;
    Ms lastMoveMs_ = 0, brakeUntil_ = 0;
    StepOutcome pending_;
};

// ---------------------------------------------------------------- spin / jog (continuous)

struct RotarySpinParams {
    Throttle throttle = 0;        // raw signed permille (motor polarity), already floored/capped
    bool     jog = false;         // lease-based, no stall exit
    Ms       stallMs = 400;
    const RotaryCharacterisation* rc = nullptr;
    Throttle floor = 400;
};

class RotarySpin {
  public:
    void start(const RotarySpinParams& p, Ticks current, Ms now) { p_ = p; slew_.reset(now); lastTicks_ = current; lastMoveMs_ = now; finished_ = false; braking_ = false; }
    StepOutcome step(const TickInputs& in, BridgeCommand& out) {
        Ticks pos = in.rotaryTicks; Ms now = in.nowMs; StepOutcome o; out = brakeCmd();
        if (finished_) return pending_;
        if (universalExit(in, in.rotaryDiag, o)) { pending_ = o; finished_ = true; return pending_; }
        if (braking_) { if (static_cast<int32_t>(now - brakeUntil_) >= 0) { finished_ = true; return pending_; } return StepOutcome::running(); }
        if (p_.jog && !in.jogLeaseLive) { pending_ = StepOutcome::done(JobResult::Done, Reason::LeaseExpired, pos); braking_ = true; brakeUntil_ = now + 50; return StepOutcome::running(); }
        if (!p_.jog) {
            Throttle cur = slew_.cur;
            if (pos != lastTicks_) { lastTicks_ = pos; lastMoveMs_ = now; }
            else if (absi(cur) >= p_.floor && elapsed(now, lastMoveMs_) >= p_.stallMs) { pending_ = StepOutcome::failed(JobResult::Stalled, Reason::Stall, pos); finished_ = true; return pending_; }
        }
        out = driveCmd(slew_.step(p_.throttle, now, *p_.rc, p_.floor));
        return StepOutcome::running();
    }
  private:
    RotarySpinParams p_;
    Slew slew_;
    Ticks lastTicks_ = 0;
    Ms lastMoveMs_ = 0, brakeUntil_ = 0;
    bool finished_ = false, braking_ = false;
    StepOutcome pending_;
};

// ---------------------------------------------------------------- home sweep

struct RotaryHomeParams {
    const RotaryCharacterisation* rc = nullptr;
    Throttle floor = 400;
    bool  referenced = false;       // a frame exists (Latched or Stale): pass 1 is possible
    Ticks tpr = 0;                  // 0 = unknown
    int   sweepDir = 1;             // tick direction of the sweep
    Ticks homeOffset = 0;           // park here (relative to the arc centre) once settled on the switch
};

struct HomeResult {
    Ticks zero = 0;                 // centre of the contact arc, in the frame the job ran in
    Ticks enter = 0, exit = 0;
    Ticks arc = 0;
    uint16_t closures = 0;          // raw make events during the contact window
    Ticks swept = 0;
    bool reversed = false;
    bool contact = false;           // switch conducting at rest when the settle pass ended
    uint8_t wiggles = 0, nudges = 0;
    uint8_t lastMoveResult = 0, lastMoveReason = 0;   // JobResult / Reason of the last settle move that did not reach its target
};

class RotaryHome {
  public:
    enum class Phase : uint8_t { Idle, Pass1, Pass1Brake, Sweep, EdgeBrake, ReverseBrake, Settle, Nudge, Offset, ParkBack, ParkIn, Finished };
    static constexpr Ms    kParkRestMs = 60;
    static constexpr Ms    kParkRestMaxMs = 400;
    static constexpr Ms    kParkMaxMs = 6000;
    static constexpr Ms    kBrakeMs = 50;
    static constexpr Ms    kSettleMs = 12;
    static constexpr Ms    kPulseMinMs = 4, kPulseMaxMs = 60;
    static constexpr Ms    kSweepBackstopMs = 25000;
    static constexpr Ms    kSettleMaxMs = 5000;      // wiggle budget
    static constexpr Ms    kNudgeMaxMs = 3500;       // contact-nudge budget on top of it
    static constexpr Ms    kInterMoveMs = 60;        // brake pause between settle moves
    static constexpr Ms    kRestConfirmMs = 90;      // at rest this long before the switch is believed
    static constexpr int   kMaxNudges = 24;
    static constexpr int   kDeepenTicks = 2;
    static constexpr Ticks kStuckClosedTicks = 60;
    static constexpr Ticks kMaxClosedTravel = 40;
    static constexpr int   kNoMovePulsesStall = 12;
    static constexpr Ticks kUnknownSweepLimit = 2000;

    void start(const RotaryHomeParams& p, Ticks current, Ms now) {
        p_ = p; res_ = HomeResult{}; sweepDir_ = p.sweepDir ? p.sweepDir : 1;
        step_ = p.tpr > 0 ? clampv<Ticks>(p.tpr / 180, 2, 1 << 20) : 4;
        limit_ = p.tpr > 0 ? p.tpr * 5 / 4 : kUnknownSweepLimit;
        pulseMs_ = kPulseMinMs;
        if (p.referenced && p.tpr > 0) {
            RotaryMoveParams m; m.rc = p.rc; m.floor = p.floor; m.speed = 500; m.maxSpeed = 500;
            Ticks offset = p.tpr * 10 / 360;                   // 10 degrees before zero, on the sweep's approach side
            // The count is unbounded after a spin (bench 2026-09-20: 8120 ticks = 6.85 turns). Every
            // multiple of a revolution is a zero, so aim at the nearest one, never at count 0 itself.
            Ticks nearestZero = current - signedTickDelta(0, current, p.tpr);
            m.target = nearestZero - sweepDir_ * offset; m.fudge = clampv<Ticks>(p.tpr * 3 / 360, 2, 1 << 20); m.stopDist = m.fudge; m.halfRev = p.tpr / 2;
            move_.start(m, current, now); phase_ = Phase::Pass1;
        } else { beginSweep(current, now); }
        phaseChanged_ = true;
    }

    StepOutcome step(const TickInputs& in, BridgeCommand& out) {
        Ticks pos = in.rotaryTicks; Ms now = in.nowMs; StepOutcome o; out = brakeCmd();
        if (phase_ == Phase::Finished) return pending_;
        if (universalExit(in, in.rotaryDiag, o)) { finish(o); return pending_; }
        switch (phase_) {
            case Phase::Pass1: {
                StepOutcome m = move_.step(in, out);
                if (m.result != StepResult::Running) { out = brakeCmd(); enter(Phase::Pass1Brake, now); brakeUntil_ = now + kBrakeMs; }
                return StepOutcome::running();
            }
            case Phase::Pass1Brake:
                if (static_cast<int32_t>(now - brakeUntil_) >= 0) beginSweep(pos, now);
                return StepOutcome::running();
            case Phase::Sweep:
                return sweep(in, out);
            case Phase::EdgeBrake:
                if (static_cast<int32_t>(now - brakeUntil_) >= 0) beginSettle(pos, now);
                return StepOutcome::running();
            case Phase::ReverseBrake:
                if (static_cast<int32_t>(now - brakeUntil_) >= 0) { beginSweep(pos, now); res_.reversed = true; }
                return StepOutcome::running();
            // Pass 3a, the settle wiggle: a decaying swing about the new zero, alternating sides,
            // with the last approach along the sweep direction so that any play in the drive
            // (Todd's motor shaft, 2026-09-20) is taken up the same way every home. It is also
            // the visible "settling into place" v3 builders liked. Best effort: it cannot change
            // the outcome, and a stall just ends it early.
            case Phase::Settle: {
                if (elapsed(now, settleStart_) >= kSettleMaxMs) { beginNudge(pos, now); return StepOutcome::running(); }
                if (!moveActive_) {
                    if (static_cast<int32_t>(now - pauseUntil_) < 0) return StepOutcome::running();
                    if (wiggleIdx_ >= nWiggle_) {
                        if (p_.rc && p_.rc->parkOnLeadingEdge) { beginPark(pos, now); return StepOutcome::running(); }
                        // Wiggle over (or abandoned). If the head is still well off the zero, one
                        // plain return move first, aimed short by the measured coast; then the nudge.
                        Ticks off = absl(pos - res_.zero);
                        if (!recovered_ && off > 4 * degTicks()) {
                            recovered_ = true; int d = res_.zero > pos ? 1 : -1;
                            moveTarget_ = res_.zero; moveDir_ = d;
                            startSettleMove(res_.zero - d * coast_, settleTol(), pos, now, true); ++res_.wiggles;
                            return StepOutcome::running();
                        }
                        beginNudge(pos, now); return StepOutcome::running();
                    }
                    Ticks target = wiggle_[wiggleIdx_]; int d = target >= pos ? 1 : -1;
                    moveTarget_ = target; moveDir_ = d;
                    startSettleMove(wiggleIdx_ == 0 ? target : target - d * coast_, settleTol(), pos, now); ++res_.wiggles;
                    return StepOutcome::running();
                }
                StepOutcome m = move_.step(in, out);
                if (m.result != StepResult::Running) {
                    out = brakeCmd(); moveActive_ = false; pauseUntil_ = now + kInterMoveMs;
                    if (m.result != StepResult::Done || m.reason != Reason::TargetReached) { res_.lastMoveResult = static_cast<uint8_t>(m.jobResult); res_.lastMoveReason = static_cast<uint8_t>(m.reason); }
                    if (wiggleIdx_ < nWiggle_) {
                        if (wiggleIdx_ == 0) {                                     // first swing: learn how far the head coasts past a brake
                            Ticks over = (pos - moveTarget_) * moveDir_;
                            coast_ = over > 0 ? over : 0;
                            if (coast_ > absl(moveTarget_ - res_.zero)) wiggleIdx_ = nWiggle_ - 2;   // coasts more than it swings: skip to the final approach (the ++ below lands on it)
                        }
                        ++wiggleIdx_;
                        if (m.result != StepResult::Done) wiggleIdx_ = nWiggle_;   // a stall or fault ends the wiggle; the nudge still runs
                    }
                }
                return StepOutcome::running();
            }
            // Pass 3b, the contact nudge (v3's settleOntoHome, made play-aware): at rest with
            // the switch open, step toward the zero one tick at a time, rest, and believe only
            // the debounced switch at rest (a moving switch chatters at the band edges, which is
            // what fooled v3's stop-on-first-contact). Reverse only when the count says the
            // head has gone a couple of degrees past. The count rides the motor shaft, so with
            // play in the drive the first steps may only take up the slack; that is why the
            // steps are small and one-directional. Bounded; the zero comes from the edge
            // centre regardless.
            case Phase::Nudge: {
                if (!moveActive_) {
                    if (static_cast<int32_t>(now - pauseUntil_) < 0) return StepOutcome::running();
                    if (in.homeSwitch) {
                        // Contact at rest. Do not stop on the very edge of the zone, where a ball
                        // bearing flickers with any vibration: go up to kDeepenTicks further in
                        // while it still conducts. Lose it and the next steps come back.
                        if (deepen_ >= kDeepenTicks || nudgeN_ >= kMaxNudges || elapsed(now, nudgeStart_) >= kNudgeMaxMs) { res_.contact = true; return settled(pos, now); }
                        ++deepen_; ++nudgeN_; ++res_.nudges; lastContactPos_ = pos;
                        startSettleMove(pos + nudgeDir_, 0, pos, now);
                        return StepOutcome::running();
                    }
                    if (deepen_ > 0) { nudgeDir_ = -nudgeDir_; deepen_ = kDeepenTicks; }        // stepped out the far side: come back and stop on the first contact
                    if (nudgeN_ >= kMaxNudges || elapsed(now, nudgeStart_) >= kNudgeMaxMs) { res_.contact = false; return settled(pos, now); }
                    if (nudgeN_ == 0) { Ticks e = res_.zero - pos; nudgeDir_ = e > 0 ? 1 : e < 0 ? -1 : sweepDir_; }
                    else if ((res_.zero - pos) * nudgeDir_ < -2 * degTicks() && nudgeFlips_ < 2) { nudgeDir_ = -nudgeDir_; ++nudgeFlips_; }
                    ++nudgeN_; ++res_.nudges;
                    startSettleMove(pos + nudgeDir_, 0, pos, now);
                    return StepOutcome::running();
                }
                StepOutcome m = move_.step(in, out);
                if (m.result != StepResult::Running) { out = brakeCmd(); moveActive_ = false; pauseUntil_ = now + kRestConfirmMs; }
                return StepOutcome::running();
            }
            // Pass 3b', the switch-only park (v3's parking spot), in two continuous moves: get
            // clear of the arc on the approach side, then move slowly back onto it and stop on
            // the first closure seen while moving, coast included. Continuous moves, not
            // pulses: on Todd's drive (2026-09-20) short pulses in one direction turn the count
            // without turning the head, while continuous moves at floor power track it. The
            // count bounds the moves; the switch decides.
            case Phase::ParkBack: {
                StepOutcome m = move_.step(in, out);
                if (m.result != StepResult::Running) { out = brakeCmd(); parkRestStart_ = now; enter(Phase::ParkIn, now); parkHit_ = false; parkMoveStarted_ = false; }
                return StepOutcome::running();
            }
            case Phase::ParkIn: {
                int approach = sweepDir_ * ((p_.rc && p_.rc->parkApproach > 0) ? 1 : -1);
                if (!parkMoveStarted_) {
                    out = brakeCmd();
                    if (elapsed(now, parkRestStart_) < kParkRestMs) return StepOutcome::running();
                    if (in.homeSwitch) {                                            // still on the arc: the back-off was not enough
                        if (parkBackTries_++ < 2) { beginParkBack(pos, now, approach, (res_.arc > 0 ? res_.arc : 40) + 20); return StepOutcome::running(); }
                        res_.contact = false; return settled(pos, now);
                    }
                    Ticks arcT = res_.arc > 0 ? res_.arc : 40;
                    startSettleMove(pos + approach * (arcT * 2 + 40), 2, pos, now);   // far enough to cross the whole arc
                    parkMoveStarted_ = true; ++res_.nudges;
                    return StepOutcome::running();
                }
                if (!parkHit_) {
                    if (in.homeSwitch) { parkHit_ = true; out = brakeCmd(); parkRestStart_ = now; return StepOutcome::running(); }
                    StepOutcome m = move_.step(in, out);
                    if (m.result != StepResult::Running) { out = brakeCmd(); res_.contact = false; return settled(pos, now); }   // crossed the bound with no closure
                    return StepOutcome::running();
                }
                out = brakeCmd();
                if (pos != parkLastTicks_) { parkLastTicks_ = pos; parkLastMoveMs_ = now; }
                if (elapsed(now, parkRestStart_) < kParkRestMs) return StepOutcome::running();
                if (elapsed(now, parkLastMoveMs_) < kParkRestMs && elapsed(now, parkRestStart_) < kParkRestMaxMs) return StepOutcome::running();
                res_.zero = pos; res_.contact = in.homeSwitch;
                return settled(pos, now);
            }
            // Pass 3c: the head is on the switch; park it where it looks straight.
            case Phase::Offset: {
                StepOutcome m = move_.step(in, out);
                if (m.result != StepResult::Running) {
                    out = brakeCmd(); moveActive_ = false;
                    if (m.result != StepResult::Done || m.reason != Reason::TargetReached) { res_.lastMoveResult = static_cast<uint8_t>(m.jobResult); res_.lastMoveReason = static_cast<uint8_t>(m.reason); }
                    finish(StepOutcome::done(JobResult::Done, Reason::None, res_.zero));
                }
                return StepOutcome::running();
            }
            default: return StepOutcome::running();
        }
    }

    Phase phase() const { return phase_; }
    bool takePhaseChanged() { bool c = phaseChanged_; phaseChanged_ = false; return c; }
    const HomeResult& result() const { return res_; }
    bool settleSkipped() const { return !res_.contact; }
    Ms pulseMs() const { return pulseMs_; }

  private:
    void enter(Phase ph, Ms now) { phase_ = ph; phaseMs_ = now; phaseChanged_ = true; }
    void finish(StepOutcome o) { pending_ = o; phase_ = Phase::Finished; phaseChanged_ = true; }
    Ticks degTicks() const { return p_.tpr > 0 ? clampv<Ticks>(p_.tpr / 360, 1, 1 << 20) : 3; }
    Ticks settleTol() const { return p_.tpr > 0 ? clampv<Ticks>(p_.tpr / 360, 2, 1 << 20) : 4; }   // one degree
    void startSettleMove(Ticks target, Ticks tol, Ticks pos, Ms now, bool allowFlip = false) {
        RotaryMoveParams m; m.rc = p_.rc; m.floor = p_.floor; m.speed = p_.floor; m.maxSpeed = p_.floor; m.target = target;
        m.fudge = tol; m.stopDist = tol; m.halfRev = p_.tpr > 0 ? p_.tpr / 2 : 590; m.allowFlip = allowFlip; m.stallMs = 400;
        move_.start(m, pos, now); moveActive_ = true;
    }
    void beginSettle(Ticks pos, Ms now) {
        settleStart_ = now; wiggleIdx_ = 0; nWiggle_ = 0; moveActive_ = false; pauseUntil_ = now; coast_ = 0; recovered_ = false;
        int amp = p_.rc ? p_.rc->settleWiggleDeg : 0;
        int s = sweepDir_;
        // degTicks() falls back to ~1 degree of a typical build when ticks/rev is not known
        // yet (bench 2026-09-20: a fresh boot with RAM-only settings skipped the wiggle).
        if (amp > 0) {
            Ticks a1 = degTicks() * amp, a2 = a1 * 6 / 10, a3 = a1 * 35 / 100;
            wiggle_[nWiggle_++] = res_.zero - s * a1;          // back against the sweep
            wiggle_[nWiggle_++] = res_.zero + s * a2;          // through zero and past it
            if (a3 >= 2) wiggle_[nWiggle_++] = res_.zero - s * a3;
            wiggle_[nWiggle_++] = res_.zero;                   // final approach along the sweep
        } else if (absl(pos - res_.zero) > settleTol()) {
            wiggle_[nWiggle_++] = res_.zero;
        }
        enter(Phase::Settle, now);
    }
    StepOutcome settled(Ticks pos, Ms now) {
        if (p_.homeOffset != 0) { startSettleMove(res_.zero + p_.homeOffset, settleTol(), pos, now, true); enter(Phase::Offset, now); return StepOutcome::running(); }
        finish(StepOutcome::done(JobResult::Done, Reason::None, res_.zero)); return pending_;
    }
    void beginPark(Ticks pos, Ms now) {
        parkStartMs_ = now; parkStartTicks_ = pos; parkBackTries_ = 0; parkHit_ = false; parkMoveStarted_ = false;
        parkLastTicks_ = pos; parkLastMoveMs_ = now;
        int approach = sweepDir_ * ((p_.rc && p_.rc->parkApproach > 0) ? 1 : -1);
        // The head is near the arc centre after the wiggle: back off a little over half an arc
        // plus margin on the approach side (further if the switch is closed right now).
        Ticks arcT = res_.arc > 0 ? res_.arc : 40;
        Ticks back = absl(pos - res_.zero) + arcT / 2 + 10;
        beginParkBack(pos, now, approach, back);
    }
    void beginParkBack(Ticks pos, Ms now, int approach, Ticks dist) {
        startSettleMove(pos - approach * dist, 3, pos, now);
        enter(Phase::ParkBack, now);
    }
    void beginNudge(Ticks pos, Ms now) { (void)pos; nudgeStart_ = now; nudgeN_ = 0; nudgeFlips_ = 0; deepen_ = 0; lastContactPos_ = 0; nudgeDir_ = sweepDir_; moveActive_ = false; pauseUntil_ = now + kRestConfirmMs; enter(Phase::Nudge, now); }
    void beginSweep(Ticks pos, Ms now) {
        enter(Phase::Sweep, now);
        sweepStartMs_ = now; swept_ = 0; sawClosure_ = false; closedTravel_ = 0; noMove_ = 0; driving_ = true;
        pulseStartMs_ = now; pulseStartTicks_ = pos; lastRaw_ = false; res_.closures = 0;
    }
    StepOutcome sweep(const TickInputs& in, BridgeCommand& out) {
        Ticks pos = in.rotaryTicks; Ms now = in.nowMs;
        // --- switch watching, every tick, both while driving and settling ---
        if (in.homeSwitchRaw && !lastRaw_ && sawClosure_) ++res_.closures;
        lastRaw_ = in.homeSwitchRaw;
        if (in.homeSwitch) {
            if (!sawClosure_) { sawClosure_ = true; res_.enter = pos; res_.closures = 1; }
            res_.exit = pos;                                       // last position seen closed
            if (absl(pos - res_.enter) > kStuckClosedTicks) { finish(StepOutcome::failed(JobResult::Failed, Reason::SwitchStuck, pos)); return pending_; }
        }
        if (sawClosure_ && (!in.homeSwitch || absl(pos - res_.enter) >= kMaxClosedTravel)) {
            res_.zero = (res_.enter + res_.exit) / 2;
            res_.arc = absl(res_.exit - res_.enter);
            res_.swept = swept_;
            out = brakeCmd(); enter(Phase::EdgeBrake, now); brakeUntil_ = now + kBrakeMs;
            return StepOutcome::running();
        }
        if (elapsed(now, sweepStartMs_) >= kSweepBackstopMs) { finish(StepOutcome::failed(JobResult::Failed, Reason::Timeout, swept_)); return pending_; }
        // --- closed-loop creep: drive for pulseMs, settle, measure, adapt ---
        if (driving_) {
            int sign = (p_.rc ? p_.rc->driveSign : 1) * sweepDir_;
            out = driveCmd(static_cast<Throttle>(sign * p_.floor));
            if (elapsed(now, pulseStartMs_) >= pulseMs_) { driving_ = false; settleStartMs_ = now; out = brakeCmd(); }
            return StepOutcome::running();
        }
        out = brakeCmd();
        if (elapsed(now, settleStartMs_) < kSettleMs) return StepOutcome::running();
        Ticks moved = absl(pos - pulseStartTicks_);
        swept_ += moved;
        // Adapt the pulse toward the target step. On a ~250-tick rotary the ideal step is only
        // two ticks, so "grow" must trigger below two ticks or a one-tick pulse would sit there
        // forever; only a pulse that moved nothing at all counts toward the stall.
        Ticks growBelow = step_ / 2 > 2 ? step_ / 2 : 2;
        if (moved < growBelow) pulseMs_ = clampv<Ms>(pulseMs_ * 3 / 2 + 1, kPulseMinMs, kPulseMaxMs);
        else if (moved > step_ * 2) pulseMs_ = clampv<Ms>(pulseMs_ * 2 / 3, kPulseMinMs, kPulseMaxMs);
        if (moved == 0) { if (++noMove_ >= kNoMovePulsesStall) { finish(StepOutcome::failed(JobResult::Stalled, Reason::Stall, swept_)); return pending_; } }
        else noMove_ = 0;
        if (swept_ > limit_ && !sawClosure_) {
            if (!res_.reversed) { sweepDir_ = -sweepDir_; res_.reversed = true; enter(Phase::ReverseBrake, now); brakeUntil_ = now + kBrakeMs; return StepOutcome::running(); }
            finish(StepOutcome::failed(JobResult::Failed, Reason::NoEdge, swept_)); return pending_;
        }
        driving_ = true; pulseStartMs_ = now; pulseStartTicks_ = pos;
        return StepOutcome::running();
    }

    RotaryHomeParams p_;
    HomeResult res_;
    RotaryMove move_;
    Phase phase_ = Phase::Idle;
    Ms phaseMs_ = 0, brakeUntil_ = 0, sweepStartMs_ = 0, pulseStartMs_ = 0, settleStartMs_ = 0, settleStart_ = 0, pulseMs_ = kPulseMinMs;
    Ms pauseUntil_ = 0, nudgeStart_ = 0;
    Ticks step_ = 4, limit_ = 0, swept_ = 0, pulseStartTicks_ = 0, closedTravel_ = 0;
    Ticks wiggle_[4] = {0, 0, 0, 0};
    Ticks coast_ = 0, moveTarget_ = 0;
    int moveDir_ = 1;
    bool recovered_ = false;
    Ms parkStartMs_ = 0, parkRestStart_ = 0, parkLastMoveMs_ = 0;
    Ticks parkStartTicks_ = 0, parkLastTicks_ = 0;
    int parkBackTries_ = 0;
    bool parkHit_ = false, parkMoveStarted_ = false;
    int sweepDir_ = 1, noMove_ = 0, nWiggle_ = 0, wiggleIdx_ = 0, nudgeN_ = 0, nudgeDir_ = 1, nudgeFlips_ = 0, deepen_ = 0;
    Ticks lastContactPos_ = 0;
    bool driving_ = true, sawClosure_ = false, lastRaw_ = false, phaseChanged_ = false, moveActive_ = false;
    StepOutcome pending_;
};

// ---------------------------------------------------------------- characterisation

struct CharacteriseResult {
    int   revs = 0;                 // revolutions measured (successive closures)
    float meanTpr = 0, minTpr = 0, maxTpr = 0;
    float spreadPct = 0;
    int   arcMean = 0, arcMin = 0, arcMax = 0;
    int   detections = 0;           // debounced closures seen
    int   closures = 0;             // raw make events (chatter included)
    Ticks lastZero = 0;             // centre of the last contact window, in the job's frame
    bool  confident = false;        // revs >= 3 and spread <= 2 %
    bool  probable = false;         // revs >= 2 and spread <= 5 %
    bool  creepFits = true;         // contact arc >= 2 * creep step
};

class RotaryCharacterise {
  public:
    static constexpr int kMaxRevs = 10;
    struct Params { const RotaryCharacterisation* rc = nullptr; Throttle floor = 400; int revs = 3; Ticks tpr = 0; };
    void start(const Params& p, Ticks current, Ms now) {
        p_ = p; if (p_.revs < 1) p_.revs = 1; if (p_.revs > kMaxRevs) p_.revs = kMaxRevs;
        res_ = CharacteriseResult{}; n_ = 0; startMs_ = now; startTicks_ = current; slew_.reset(now);
        closed_ = false; lastRaw_ = false; finished_ = false; braking_ = false; firstSeen_ = false;
        deadline_ = static_cast<Ms>(p_.revs) * 20000u + 10000u;
        firstLimit_ = p.tpr > 0 ? p.tpr * 5 / 4 : 2000;
    }
    StepOutcome step(const TickInputs& in, BridgeCommand& out) {
        Ticks pos = in.rotaryTicks; Ms now = in.nowMs; StepOutcome o; out = brakeCmd();
        if (finished_) return pending_;
        if (universalExit(in, in.rotaryDiag, o)) { pending_ = o; finished_ = true; return pending_; }
        if (braking_) { if (static_cast<int32_t>(now - brakeUntil_) >= 0) { finished_ = true; return pending_; } return StepOutcome::running(); }
        if (in.homeSwitchRaw && !lastRaw_ && closed_) ++res_.closures;
        lastRaw_ = in.homeSwitchRaw;
        if (in.homeSwitch && !closed_) {
            closed_ = true; enter_ = pos; ++res_.detections; if (!firstSeen_) { firstSeen_ = true; res_.closures = 1; } else ++res_.closures;
            if (res_.detections > 1) { float rev = static_cast<float>(absl(enter_ - lastEnter_)); if (n_ < kMaxRevs) tprs_[n_++] = rev; }
            lastEnter_ = enter_;
        }
        if (in.homeSwitch) exit_ = pos;
        if (!in.homeSwitch && closed_) {
            closed_ = false;
            int arc = static_cast<int>(absl(exit_ - enter_)); if (n_ <= kMaxRevs) { arcs_[na_ < kMaxRevs ? na_ : kMaxRevs - 1] = arc; if (na_ < kMaxRevs) ++na_; }
            res_.lastZero = (enter_ + exit_) / 2;
            if (n_ >= p_.revs) { finishUp(); pending_ = StepOutcome::done(JobResult::Done, Reason::None, res_.lastZero); braking_ = true; brakeUntil_ = now + 50; return StepOutcome::running(); }
        }
        if (!firstSeen_ && absl(pos - startTicks_) > firstLimit_) { pending_ = StepOutcome::failed(JobResult::Failed, Reason::NoEdge, absl(pos - startTicks_)); finished_ = true; return pending_; }
        if (elapsed(now, startMs_) >= deadline_) { pending_ = StepOutcome::failed(JobResult::Failed, Reason::Timeout, 0); finished_ = true; return pending_; }
        int sign = p_.rc ? p_.rc->driveSign : 1;
        out = driveCmd(slew_.step(static_cast<Throttle>(sign * (p_.floor + 100)), now, *p_.rc, p_.floor));
        return StepOutcome::running();
    }
    const CharacteriseResult& result() const { return res_; }
  private:
    void finishUp() {
        res_.revs = n_;
        if (n_ > 0) {
            float sum = 0; res_.minTpr = tprs_[0]; res_.maxTpr = tprs_[0];
            for (int i = 0; i < n_; ++i) { sum += tprs_[i]; if (tprs_[i] < res_.minTpr) res_.minTpr = tprs_[i]; if (tprs_[i] > res_.maxTpr) res_.maxTpr = tprs_[i]; }
            res_.meanTpr = sum / n_;
            res_.spreadPct = res_.meanTpr > 0 ? (res_.maxTpr - res_.minTpr) * 100.f / res_.meanTpr : 100.f;
            res_.confident = n_ >= 3 && res_.spreadPct <= 2.f;
            res_.probable = n_ >= 2 && res_.spreadPct <= 5.f;
        }
        if (na_ > 0) {
            int sum = 0; res_.arcMin = arcs_[0]; res_.arcMax = arcs_[0];
            for (int i = 0; i < na_; ++i) { sum += arcs_[i]; if (arcs_[i] < res_.arcMin) res_.arcMin = arcs_[i]; if (arcs_[i] > res_.arcMax) res_.arcMax = arcs_[i]; }
            res_.arcMean = sum / na_;
        }
        Ticks step = res_.meanTpr > 0 ? clampv<Ticks>(static_cast<Ticks>(res_.meanTpr) / 180, 2, 1 << 20) : 4;
        res_.creepFits = res_.arcMean >= 2 * step;
    }
    Params p_;
    CharacteriseResult res_;
    Slew slew_;
    float tprs_[kMaxRevs] = {};
    int arcs_[kMaxRevs] = {};
    int n_ = 0, na_ = 0;
    Ms startMs_ = 0, deadline_ = 0, brakeUntil_ = 0;
    Ticks startTicks_ = 0, enter_ = 0, exit_ = 0, lastEnter_ = 0, firstLimit_ = 0;
    bool closed_ = false, lastRaw_ = false, finished_ = false, braking_ = false, firstSeen_ = false;
    StepOutcome pending_;
};

} // namespace uppity
