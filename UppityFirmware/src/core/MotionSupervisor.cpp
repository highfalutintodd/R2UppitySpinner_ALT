#include "MotionSupervisor.h"

namespace uppity {

MotionSupervisor::MotionSupervisor(Hal& hal, EstopLatch& estop, CommandQueue& queue, SnapshotPublisher& pub, EventLog& events)
    : hal_(hal), estop_(estop), queue_(queue), pub_(pub), events_(events), prof_(&profileFor(cfg_.motor)) {
    lastOut_[0] = brakeCmd(); lastOut_[1] = brakeCmd();
}

void MotionSupervisor::setConfig(const MotionConfig& c) { pendingCfg_ = c; cfgPending_ = true; }

void MotionSupervisor::applyPendingConfig() {
    if (!cfgPending_ || job_ != JobKind::None) return;
    cfgPending_ = false;
    cfg_ = pendingCfg_;
    prof_ = &profileFor(cfg_.motor);
    lifter_.travelTicks = travel();
    lifter_.travelCalibrated = cfg_.travelCalibrated;
}

Ticks MotionSupervisor::travel() const { return cfg_.travelTicks > 0 ? cfg_.travelTicks : static_cast<Ticks>(prof_->defaultTravelTicks); }
Ticks MotionSupervisor::safeSpin() const { return cfg_.safeSpinTicks > 0 ? cfg_.safeSpinTicks : travel() / 2; }
Ticks MotionSupervisor::tpr() const { return cfg_.rotary.ticksPerRev > 0 ? static_cast<Ticks>(cfg_.rotary.ticksPerRev + 0.5f) : 0; }

Throttle MotionSupervisor::floorSpeed(Throttle speed, int dir) const {
    Throttle floor = dir > 0 ? cfg_.minPowerUp : cfg_.minPowerDown;
    if (floor <= 0) floor = pctToThrottle(prof_->defaultMinPowerPct);
    Throttle cap = effectiveMaxThrottle(cfg_.motor, cfg_.expertMode);
    if (speed <= 0) speed = cap;
    if (speed < floor) speed = floor;
    if (speed > cap) speed = cap;
    return speed;
}

void MotionSupervisor::begin(Ms now) {
    applyPendingConfig();
    RawInputs raw; hal_.readInputs(raw);
    BootResolution r = bootResolve(raw.topLimit, raw.bottomLimit, raw.homeSwitch, travel());
    lifter_.confidence = r.lifter;
    lifter_.travelTicks = travel();
    if (r.lifter == LifterConfidence::Known) { hal_.setEncoder(Axis::Lifter, r.lifterTicks); lifter_.ticks = r.lifterTicks; lifter_.referencedAt = now; }
    if (r.contradiction) faults_.set(F_LimitContradiction);
    if (r.rotaryRestContact && !cfg_.rotaryDisabled) {
        hal_.setEncoder(Axis::Rotary, 0);
        rotary_.ticks = 0; rotary_.confidence = HomeConfidence::SwitchLive; rotary_.provenance = HomeProvenance::RestContact; rotary_.referencedAt = now;
    }
    top_.state = raw.topLimit; bottom_.state = raw.bottomLimit; home_.state = raw.homeSwitch;
    topWas_ = raw.topLimit; botWas_ = raw.bottomLimit; homeWas_ = raw.homeSwitch;
    hal_.requestEnable(false);
    idleSince_ = now;
    state_ = cfg_.commissioned ? MotionState::Idle : MotionState::Locked;
    int limits = (raw.topLimit ? 1 : 0) | (raw.bottomLimit ? 2 : 0) | (raw.homeSwitch ? 4 : 0);
    emit(Ev::Boot, static_cast<int32_t>(lifter_.confidence), static_cast<int32_t>(rotary_.confidence), limits, now);
    TickInputs in; in.nowMs = now; in.topLimit = raw.topLimit; in.bottomLimit = raw.bottomLimit; in.homeSwitch = raw.homeSwitch;
    publish(in, brakeCmd(), brakeCmd());
}

void MotionSupervisor::debounceSwitch(Debounce& d, bool raw, int makeTicks, int breakTicks) {
    if (raw) { d.brk = 0; if (d.make < 255) ++d.make; if (d.make >= makeTicks) d.state = true; }
    else     { d.make = 0; if (d.brk < 255) ++d.brk; if (d.brk >= breakTicks) d.state = false; }
}

PolicyContext MotionSupervisor::policy(const TickInputs& in) const {
    PolicyContext c;
    c.lifter = lifter_; c.rotary = rotary_;
    c.ticksPerRev = cfg_.rotary.ticksPerRev;
    c.safeSpinTicks = safeSpin();
    c.rotaryDisabled = cfg_.rotaryDisabled;
    c.estop = in.estop;
    c.locked = !cfg_.commissioned && !cfg_.commissioningActive;
    c.lifterFault = faults_.lifterBlocked();
    c.rotaryFault = faults_.rotaryBlocked();
    c.globalFault = faults_.globalBlocked();
    c.overrideLive = queue_.overrideLive(in.nowMs);
    c.topLimit = in.topLimit; c.bottomLimit = in.bottomLimit; c.homeSwitch = in.homeSwitch;
    return c;
}

void MotionSupervisor::updateKnowledge(const TickInputs& in, bool rotaryBusy) {
    // Limit switches re-reference the lifter on their making edge.
    if (in.topLimit && !topWas_) {
        Ticks want = travel();
        if (!suppressLimitRef_) {
            if (lifter_.confidence == LifterConfidence::Known && lifter_.travelCalibrated && absl(in.lifterTicks - want) > travel() / 4) faults_.set(F_LimitMismatch);
            hal_.setEncoder(Axis::Lifter, want); lifter_.ticks = want;
        }
        lifter_.confidence = LifterConfidence::Known; lifter_.referencedAt = in.nowMs;
        emit(Ev::LimitTop, 1, in.lifterTicks, want, in.nowMs);
    } else if (!in.topLimit && topWas_) emit(Ev::LimitTop, 0, in.lifterTicks, 0, in.nowMs);
    if (in.bottomLimit && !botWas_) {
        if (!suppressLimitRef_) {
            if (lifter_.confidence == LifterConfidence::Known && lifter_.travelCalibrated && absl(in.lifterTicks) > travel() / 4) faults_.set(F_LimitMismatch);
            hal_.setEncoder(Axis::Lifter, 0); lifter_.ticks = 0;
        }
        lifter_.confidence = LifterConfidence::Known; lifter_.referencedAt = in.nowMs;
        emit(Ev::LimitBottom, 1, in.lifterTicks, 0, in.nowMs);
    } else if (!in.bottomLimit && botWas_) emit(Ev::LimitBottom, 0, in.lifterTicks, 0, in.nowMs);
    topWas_ = in.topLimit; botWas_ = in.bottomLimit;

    if (in.homeSwitch != homeWas_) emit(Ev::HomeSwitch, in.homeSwitch ? 1 : 0, in.rotaryTicks, 0, in.nowMs);
    homeWas_ = in.homeSwitch;

    // Rotary rest contact: a closed switch with the motor at rest is a trusted zero.
    if (!cfg_.rotaryDisabled && !rotaryBusy) {
        if (in.homeSwitch) {
            if (rotary_.confidence == HomeConfidence::Latched && tpr() > 0) {
                Ticks err = signedTickDelta(0, in.rotaryTicks, tpr());
                if (absl(err) > homeToleranceTicks(cfg_.rotary.ticksPerRev)) { hal_.setEncoder(Axis::Rotary, 0); rotary_.ticks = 0; emit(Ev::HomeFrameCorrected, err, 0, 0, in.nowMs); }
            } else if (rotary_.confidence != HomeConfidence::SwitchLive) {
                hal_.setEncoder(Axis::Rotary, 0); rotary_.ticks = 0;
            }
            if (rotary_.confidence != HomeConfidence::SwitchLive) emit(Ev::ConfidenceRotary, static_cast<int32_t>(rotary_.confidence), static_cast<int32_t>(HomeConfidence::SwitchLive), 0, in.nowMs);
            rotary_.confidence = HomeConfidence::SwitchLive; rotary_.provenance = HomeProvenance::RestContact; rotary_.referencedAt = in.nowMs;
        } else if (rotary_.confidence == HomeConfidence::SwitchLive) {
            rotary_.confidence = HomeConfidence::Latched;
            emit(Ev::ConfidenceRotary, static_cast<int32_t>(HomeConfidence::SwitchLive), static_cast<int32_t>(HomeConfidence::Latched), 0, in.nowMs);
        }
    }
}

// ---------------------------------------------------------------- programs

bool MotionSupervisor::lifterAboveSafeNow(const TickInputs& in) const {
    return in.topLimit || (lifter_.confidence != LifterConfidence::Unknown && in.lifterTicks > safeSpin());
}

int MotionSupervisor::expandRequest(const MotionRequest& r, const TickInputs& in, Step* steps) {
    int n = 0;
    auto add = [&](JobKind k, int32_t a, int32_t b, int32_t c) { if (n < kMaxSteps) { steps[n].kind = k; steps[n].a = a; steps[n].b = b; steps[n].c = c; ++n; } };
    const int32_t kSkipKnown = 2, kSkipHome = 2, kSkipBelowSafe = 4;
    bool raiseFirst = !cfg_.rotaryDisabled && !lifterAboveSafeNow(in);
    switch (r.kind) {
        case JobKind::RotaryAbsolute:
            if (raiseFirst) add(JobKind::LifterSeek, travel(), 0, 1);
            if (tpr() <= 0) add(JobKind::RotaryCharacterise, 2, 0, 0);
            else if (!rotaryHomeKnown()) add(JobKind::RotaryHome, 0, 0, 0);
            add(JobKind::RotaryAbsolute, r.a, r.b, r.c);
            break;
        case JobKind::RotaryRelative:
        case JobKind::RotarySpin:
        case JobKind::RotaryHome:
        case JobKind::RotaryCharacterise:
            if (raiseFirst) add(JobKind::LifterSeek, travel(), 0, 1);
            if (r.kind == JobKind::RotaryRelative && tpr() <= 0) add(JobKind::RotaryCharacterise, 2, 0, 0);
            add(r.kind, r.a, r.b, r.c);
            break;
        case JobKind::SafeLower:
            add(JobKind::LifterSeek, travel(), 0, 1 | kSkipKnown);                       // reference if unknown
            if (!cfg_.rotaryDisabled) {
                if (tpr() <= 0) add(JobKind::RotaryCharacterise, 2, 0, kSkipHome | kSkipBelowSafe);
                else add(JobKind::RotaryHome, 0, 0, kSkipHome | kSkipBelowSafe);
            }
            add(JobKind::LifterSeek, 0, r.b, 0);
            break;
        case JobKind::LifterCalibrate:
            if (!cfg_.rotaryDisabled && !rotaryHomeKnown()) {
                add(JobKind::LifterSeek, travel(), 0, 1);
                if (tpr() <= 0) add(JobKind::RotaryCharacterise, 2, 0, 0); else add(JobKind::RotaryHome, 0, 0, 0);
            }
            add(JobKind::LifterCalibrate, 0, 0, 0);
            break;
        default:
            add(r.kind, r.a, r.b, r.c);
            break;
    }
    return n;
}

void MotionSupervisor::refuse(const Step& s, Reason why, uint32_t id, const TickInputs& in) {
    emit(Ev::Refused, static_cast<int32_t>(s.kind), static_cast<int32_t>(why), static_cast<int32_t>(id), in.nowMs);
    lastJob_ = s.kind; lastResult_ = JobResult::Refused; lastReason_ = why;
}

int MotionSupervisor::startStep(const Step& s, const TickInputs& in) {
    PolicyContext c = policy(in);
    switch (s.kind) {
        case JobKind::LifterSeek: {
            if ((s.c & 2) && lifter_.confidence != LifterConfidence::Unknown) return 0;   // skip: already referenced
            Ticks target = clampv<Ticks>(s.a, 0, travel());
            if (programKind_ == JobKind::SafeLower && target == 0) emit(Ev::LightKit, 1, 0, 0, in.nowMs);   // lights off before lowering
            Decision d = checkLifterSeek(c, in.lifterTicks, target);
            if (!d.ok()) { refuse(s, d.reason, programId_, in); return -1; }
            if (d.verdict == Verdict::Clamp) { emit(Ev::SeekClamped, target, d.value, static_cast<int32_t>(d.reason), in.nowMs); target = d.value; }
            int dir = target >= in.lifterTicks ? 1 : -1;
            SeekParams p;
            p.target = target;
            p.speed = floorSpeed(static_cast<Throttle>(s.b), dir);
            p.approachPower = cfg_.approachPower > 0 ? cfg_.approachPower : pctToThrottle(prof_->defaultSeekBottomPowerPct);
            p.fullTravel = (s.c & 1) != 0 && d.verdict != Verdict::Clamp;
            { int slot = clampv<int>(p.speed / 50, 0, kSlots - 1); p.stopComp = dir > 0 ? cfg_.overshootUp[slot] : cfg_.overshootDown[slot]; }
            p.prof = prof_;
            if (!p.fullTravel && absl(target - in.lifterTicks) < 5) { lastReason_ = Reason::AlreadyThere; emit(Ev::JobEnd, static_cast<int32_t>(s.kind), static_cast<int32_t>(JobResult::Done), static_cast<int32_t>(Reason::AlreadyThere), in.nowMs); return 0; }
            if (p.fullTravel && ((target == 0 && in.bottomLimit) || (target == travel() && in.topLimit))) { lastReason_ = Reason::AlreadyThere; return 0; }
            seek_.start(p, in.lifterTicks, in.nowMs);
            job_ = JobKind::LifterSeek;
            emit(Ev::JobStart, static_cast<int32_t>(job_), target, static_cast<int32_t>(programId_), in.nowMs);
            return 1;
        }
        case JobKind::LifterCreep: {
            int dir = s.a < 0 ? -1 : 1;
            Decision d = checkLifterCreep(c, dir);
            if (!d.ok()) { refuse(s, d.reason, programId_, in); return -1; }
            CreepParams p;
            p.dir = dir;
            p.jog = (s.c & 1) != 0;
            Throttle cap = effectiveMaxThrottle(cfg_.motor, cfg_.expertMode);
            Throttle req = static_cast<Throttle>(s.b);
            if (p.jog) p.power = clampv<Throttle>(req, 120, cap);
            else p.power = clampv<Throttle>(req > 0 ? req : pctToThrottle(prof_->defaultMinPowerPct), pctToThrottle(prof_->defaultMinPowerPct), cap);
            p.floorTicks = d.value;
            creep_.start(p, in.lifterTicks, in.nowMs);
            job_ = JobKind::LifterCreep;
            emit(Ev::JobStart, static_cast<int32_t>(job_), dir, static_cast<int32_t>(programId_), in.nowMs);
            return 1;
        }
        case JobKind::RotaryAbsolute:
        case JobKind::RotaryRelative: {
            Decision d = checkRotary(c, false);
            if (!d.ok()) { refuse(s, d.reason, programId_, in); return -1; }
            if (tpr() <= 0) { refuse(s, Reason::NotRepeatable, programId_, in); return -1; }
            Ticks target;
            if (s.kind == JobKind::RotaryAbsolute) {
                if (!rotaryHomeKnown()) { refuse(s, Reason::RotaryNotHome, programId_, in); return -1; }
                int16_t curDeg = normalizeDeg(static_cast<int32_t>(cfg_.rotary.ticksToDeg(in.rotaryTicks)));
                int16_t delta = signedCircularDelta(curDeg, normalizeDeg(s.a));
                target = in.rotaryTicks + cfg_.rotary.degToTicks(static_cast<float>(delta));
            } else {
                target = in.rotaryTicks + cfg_.rotary.degToTicks(static_cast<float>(s.a));
            }
            RotaryMoveParams p;
            p.rc = &cfg_.rotary; p.floor = rotaryFloor();
            p.speed = s.b > 0 ? clampv<Throttle>(static_cast<Throttle>(s.b), p.floor, kThrottleMax) : p.floor;
            p.maxSpeed = s.c > 0 ? clampv<Throttle>(static_cast<Throttle>(s.c), p.speed, kThrottleMax) : p.speed;
            p.target = target;
            p.fudge = clampv<Ticks>(tpr() * 5 / 360, 2, 1 << 20);
            p.stopDist = cfg_.rotary.stopDistanceTicks > 0 ? cfg_.rotary.stopDistanceTicks : clampv<Ticks>(tpr() / 72, p.fudge, 1 << 20);
            p.halfRev = tpr() / 2;
            rmove_.start(p, in.rotaryTicks, in.nowMs);
            job_ = s.kind;
            emit(Ev::JobStart, static_cast<int32_t>(job_), target, static_cast<int32_t>(programId_), in.nowMs);
            return 1;
        }
        case JobKind::RotarySpin:
        case JobKind::RotaryJog: {
            bool jog = s.kind == JobKind::RotaryJog;
            Decision d = checkRotary(c, jog);
            if (!d.ok()) { refuse(s, d.reason, programId_, in); return -1; }
            RotarySpinParams p;
            p.rc = &cfg_.rotary; p.jog = jog;
            p.floor = jog ? pctToThrottle(cfg_.rotary.jogFloorPct) : rotaryFloor();
            Throttle floor = p.floor;
            Throttle mag = clampv<Throttle>(static_cast<Throttle>(absi(s.a)), floor, kThrottleMax);
            p.throttle = static_cast<Throttle>(s.a < 0 ? -mag : mag);
            rspin_.start(p, in.rotaryTicks, in.nowMs);
            job_ = s.kind;
            emit(Ev::JobStart, static_cast<int32_t>(job_), p.throttle, static_cast<int32_t>(programId_), in.nowMs);
            return 1;
        }
        case JobKind::RotaryHome: {
            if ((s.c & 2) && (rotaryHomeKnown() || cfg_.rotaryDisabled)) return 0;
            if ((s.c & 4) && lifter_.confidence != LifterConfidence::Unknown && in.lifterTicks <= safeSpin() && !in.topLimit) return 0;
            Decision d = checkRotary(c, false);
            if (!d.ok()) { refuse(s, d.reason, programId_, in); return -1; }
            RotaryHomeParams p;
            p.rc = &cfg_.rotary; p.floor = rotaryFloor(); p.referenced = rotaryReferenced(); p.tpr = tpr(); p.sweepDir = 1;
            rhome_.start(p, in.rotaryTicks, in.nowMs);
            job_ = JobKind::RotaryHome;
            emit(Ev::JobStart, static_cast<int32_t>(job_), p.referenced ? 1 : 0, static_cast<int32_t>(programId_), in.nowMs);
            return 1;
        }
        case JobKind::RotaryCharacterise: {
            if ((s.c & 2) && (rotaryHomeKnown() || cfg_.rotaryDisabled)) return 0;
            if ((s.c & 4) && lifter_.confidence != LifterConfidence::Unknown && in.lifterTicks <= safeSpin() && !in.topLimit) return 0;
            Decision d = checkRotary(c, false);
            if (!d.ok()) { refuse(s, d.reason, programId_, in); return -1; }
            RotaryCharacterise::Params p;
            p.rc = &cfg_.rotary; p.floor = rotaryFloor(); p.revs = s.a > 0 ? s.a : 3; p.tpr = tpr();
            rchar_.start(p, in.rotaryTicks, in.nowMs);
            job_ = JobKind::RotaryCharacterise;
            emit(Ev::JobStart, static_cast<int32_t>(job_), p.revs, static_cast<int32_t>(programId_), in.nowMs);
            return 1;
        }
        case JobKind::LifterCalibrate: {
            Decision d = gateCommon(c);
            if (!d.ok()) { refuse(s, d.reason, programId_, in); return -1; }
            if (c.lifterFault) { refuse(s, Reason::DiagFault, programId_, in); return -1; }
            if (!(rotaryIsHome(c) || cfg_.rotaryDisabled)) { refuse(s, Reason::RotaryNotHome, programId_, in); return -1; }
            CalibrateParams p;
            p.prof = prof_;
            p.sweepCap = pctToThrottle(effectiveSweepCapPct(cfg_.motor, cfg_.expertMode));
            p.startPower = pctToThrottle(prof_->defaultMinPowerPct);
            p.cooldownMs = prof_->calibrationCooldownMs;
            p.travelHint = travel();
            calib_.start(p, in.lifterTicks, in.nowMs);
            suppressLimitRef_ = true;
            holdTarget_ = -1;
            job_ = JobKind::LifterCalibrate;
            emit(Ev::JobStart, static_cast<int32_t>(job_), 0, static_cast<int32_t>(programId_), in.nowMs);
            return 1;
        }
        default:
            refuse(s, Reason::None, programId_, in);
            return -1;
    }
}

// Start the next runnable step of the current program; skipped steps fall through.
void MotionSupervisor::runProgram(const TickInputs& in) {
    while (job_ == JobKind::None && stepIdx_ < stepCount_) {
        int r = startStep(steps_[stepIdx_], in);
        if (r == 1) return;
        if (r == 0) { ++stepIdx_; continue; }
        StepOutcome o = StepOutcome::failed(JobResult::Refused, lastReason_);
        stepCount_ = 0; endProgram(o, in);
        return;
    }
    if (job_ == JobKind::None && stepCount_ > 0) endProgram(StepOutcome::done(JobResult::Done, lastReason_ == Reason::AlreadyThere ? Reason::AlreadyThere : Reason::None), in);
}

void MotionSupervisor::stepJob(const TickInputs& in, BridgeCommand& lifterOut, BridgeCommand& rotaryOut) {
    StepOutcome o;
    switch (job_) {
        case JobKind::LifterSeek:
            o = seek_.step(in, lifterOut);
            if (seek_.takePhaseChanged()) emit(Ev::Phase, static_cast<int32_t>(job_), static_cast<int32_t>(seek_.phase()), in.lifterTicks, in.nowMs);
            break;
        case JobKind::LifterCreep: o = creep_.step(in, lifterOut); break;
        case JobKind::RotaryAbsolute:
        case JobKind::RotaryRelative: o = rmove_.step(in, rotaryOut); break;
        case JobKind::RotarySpin:
        case JobKind::RotaryJog: o = rspin_.step(in, rotaryOut); break;
        case JobKind::RotaryHome:
            o = rhome_.step(in, rotaryOut);
            if (rhome_.takePhaseChanged()) emit(Ev::Phase, static_cast<int32_t>(job_), static_cast<int32_t>(rhome_.phase()), in.rotaryTicks, in.nowMs);
            break;
        case JobKind::RotaryCharacterise: o = rchar_.step(in, rotaryOut); break;
        case JobKind::LifterCalibrate:
            o = calib_.step(in, lifterOut);
            if (calib_.takePhaseChanged()) emit(Ev::Calibration, static_cast<int32_t>(calib_.phase()), in.lifterTicks, 0, in.nowMs);
            break;
        default: o = StepOutcome::failed(JobResult::Failed, Reason::None); break;
    }
    if (o.result != StepResult::Running) { lifterOut = brakeCmd(); rotaryOut = brakeCmd(); endStep(o, in); }
}

void MotionSupervisor::endStep(const StepOutcome& o, const TickInputs& in) {
    JobKind k = job_;
    emit(Ev::JobEnd, static_cast<int32_t>(k), static_cast<int32_t>(o.jobResult), static_cast<int32_t>(o.reason), in.nowMs);

    // ---- knowledge effects ----
    if (k == JobKind::LifterSeek || k == JobKind::LifterCreep || k == JobKind::LifterCalibrate) {
        if (o.jobResult == JobResult::Stalled) { faults_.set(F_LifterStall); if (lifter_.confidence == LifterConfidence::Known) lifter_.confidence = LifterConfidence::Stale; }
        if (o.reason == Reason::EncoderDirection) { faults_.set(F_EncoderDirection); lifter_.confidence = LifterConfidence::Stale; }
        if (o.jobResult == JobResult::DoneLimit) faults_.clear(F_LifterStall);
    }
    // hold: a completed seek to a mid-travel target is held against drift; anything else drops it
    if (k == JobKind::LifterSeek && programKind_ != JobKind::LifterHold) {
        if (o.jobResult == JobResult::DoneTarget && seek_.target() > travel() / 100) holdTarget_ = seek_.target(), holdFails_ = 0, holdCheckAt_ = in.nowMs;
        else holdTarget_ = -1;
    } else if (k == JobKind::LifterCreep || k == JobKind::LifterCalibrate) holdTarget_ = -1;
    if (k == JobKind::LifterCalibrate) {
        suppressLimitRef_ = false;
        if (o.result == StepResult::Done) {
            lastCal_ = calib_.result();
            cfg_.travelTicks = lastCal_.travel; cfg_.travelCalibrated = true;
            cfg_.minPowerUp = lastCal_.minUp; cfg_.minPowerDown = lastCal_.minDown;
            for (int i = 0; i < kSlots; ++i) { cfg_.overshootUp[i] = lastCal_.overUp[i]; cfg_.overshootDown[i] = lastCal_.overDown[i]; cfg_.velUp[i] = lastCal_.velUp[i]; cfg_.velDown[i] = lastCal_.velDown[i]; }
            if (cfgPending_) { pendingCfg_.travelTicks = cfg_.travelTicks; pendingCfg_.travelCalibrated = true; pendingCfg_.minPowerUp = cfg_.minPowerUp; pendingCfg_.minPowerDown = cfg_.minPowerDown; }
            lifter_.travelTicks = cfg_.travelTicks; lifter_.travelCalibrated = true;
            calUpdated_ = true;
            emit(Ev::Measure, 3, lastCal_.travel, static_cast<int32_t>(lastCal_.travelSpreadPct * 10), in.nowMs);
            emit(Ev::Measure, 4, lastCal_.minUp, lastCal_.minDown, in.nowMs);
        }
        // the machine ends at the bottom limit: re-reference there
        if (in.bottomLimit) { hal_.setEncoder(Axis::Lifter, 0); lifter_.ticks = 0; lifter_.confidence = LifterConfidence::Known; }
    }
    if (isRotaryJob(k)) {
        if (o.jobResult == JobResult::Stalled) { faults_.set(F_RotaryStall); if (rotary_.confidence == HomeConfidence::Latched) { rotary_.confidence = HomeConfidence::Stale; emit(Ev::ConfidenceRotary, static_cast<int32_t>(HomeConfidence::Latched), static_cast<int32_t>(HomeConfidence::Stale), 0, in.nowMs); } }
    }
    if (k == JobKind::RotaryHome) {
        if (o.result == StepResult::Done) {
            lastHome_ = rhome_.result();
            Ticks zero = lastHome_.zero;
            hal_.setEncoder(Axis::Rotary, in.rotaryTicks - zero);
            rotary_.ticks = in.rotaryTicks - zero;
            HomeConfidence was = rotary_.confidence;
            rotary_.confidence = HomeConfidence::Latched; rotary_.provenance = HomeProvenance::EdgeCentre; rotary_.referencedAt = in.nowMs;
            rotary_.lastEdgeArcTicks = lastHome_.arc; rotary_.lastEdgeClosures = static_cast<uint8_t>(lastHome_.closures > 255 ? 255 : lastHome_.closures);
            faults_.clear(F_RotaryStall);
            emit(Ev::HomeEdge, lastHome_.enter, lastHome_.exit, lastHome_.closures, in.nowMs);
            if (was != HomeConfidence::Latched) emit(Ev::ConfidenceRotary, static_cast<int32_t>(was), static_cast<int32_t>(HomeConfidence::Latched), 0, in.nowMs);
        } else if (rotary_.confidence == HomeConfidence::Latched) {
            rotary_.confidence = HomeConfidence::Stale;   // frame untouched, trust reduced
            emit(Ev::ConfidenceRotary, static_cast<int32_t>(HomeConfidence::Latched), static_cast<int32_t>(HomeConfidence::Stale), 0, in.nowMs);
        }
    }
    if (k == JobKind::RotaryCharacterise && o.result == StepResult::Done) {
        lastChar_ = rchar_.result();
        emit(Ev::Measure, 1, static_cast<int32_t>(lastChar_.meanTpr * 10.f), static_cast<int32_t>(lastChar_.spreadPct * 10.f), in.nowMs);
        emit(Ev::Measure, 2, lastChar_.arcMean, lastChar_.closures, in.nowMs);
        if (lastChar_.confident || lastChar_.probable) {
            cfg_.rotary.ticksPerRev = lastChar_.meanTpr;
            cfg_.rotary.confident = lastChar_.confident;
            cfg_.rotary.contactArcTicks = static_cast<uint16_t>(lastChar_.arcMean);
            cfg_.rotary.chatterPerPass = static_cast<uint8_t>(lastChar_.detections > 0 ? clampv<int>(lastChar_.closures / lastChar_.detections, 0, 255) : 0);
            charUpdated_ = true;
            if (cfgPending_) pendingCfg_.rotary = cfg_.rotary;
        }
        Ticks zero = lastChar_.lastZero;
        hal_.setEncoder(Axis::Rotary, in.rotaryTicks - zero);
        rotary_.ticks = in.rotaryTicks - zero;
        rotary_.confidence = HomeConfidence::Latched; rotary_.provenance = HomeProvenance::EdgeCentre; rotary_.referencedAt = in.nowMs;
        rotary_.lastEdgeArcTicks = lastChar_.arcMean;
    }

    job_ = JobKind::None; arming_ = false;

    // ---- program continuation ----
    if (o.result == StepResult::Done && stepIdx_ + 1 < stepCount_) {
        ++stepIdx_;
        runProgram(in);
        return;
    }
    if (o.result != StepResult::Done && programKind_ == JobKind::SafeLower && (k == JobKind::RotaryHome || k == JobKind::RotaryCharacterise)) {
        StepOutcome h = o; h.reason = Reason::HomeFailed; stepCount_ = 0; endProgram(h, in); return;
    }
    stepCount_ = 0;
    endProgram(o, in);
}

void MotionSupervisor::endProgram(const StepOutcome& o, const TickInputs& in) {
    if (job_ != JobKind::None) return;      // a next step started
    if (programKind_ == JobKind::LifterHold) {
        if (o.result == StepResult::Done) holdFails_ = 0;
        else if (++holdFails_ >= 3) { emit(Ev::Hold, 0, holdTarget_, in.lifterTicks, in.nowMs); holdTarget_ = -1; }
        holdCheckAt_ = in.nowMs;
    } else { lastJob_ = programKind_; lastResult_ = o.jobResult; lastReason_ = o.reason; }
    stepCount_ = 0; stepIdx_ = 0; programKind_ = JobKind::None; programId_ = 0;
    idleSince_ = in.nowMs;
}

void MotionSupervisor::abortJob(JobResult how, Reason why, const TickInputs& in) {
    if (job_ == JobKind::None) return;
    StepOutcome o = StepOutcome::failed(how, why);
    stepCount_ = stepIdx_ + 1;                 // no continuation after an abort
    endStep(o, in);
}

void MotionSupervisor::refuseAll(Reason why, const TickInputs& in) {
    MotionRequest r;
    while (queue_.pop(r)) {
        emit(Ev::Refused, static_cast<int32_t>(r.kind), static_cast<int32_t>(why), static_cast<int32_t>(r.id), in.nowMs);
        lastJob_ = r.kind; lastResult_ = JobResult::Refused; lastReason_ = why;
    }
}

// ---------------------------------------------------------------- the tick

void MotionSupervisor::tick(Ms now) {
    ++tickCount_;
    applyPendingConfig();

    RawInputs raw; hal_.readInputs(raw);
    debounceSwitch(top_, raw.topLimit, kLimitMakeTicks, kLimitBreakTicks);
    debounceSwitch(bottom_, raw.bottomLimit, kLimitMakeTicks, kLimitBreakTicks);
    debounceSwitch(home_, raw.homeSwitch, kHomeMakeTicks, kHomeBreakTicks);

    bool enabled = raw.enableAcked;
    if (enabled && !lastEnabled_) enabledSince_ = now;
    lastEnabled_ = enabled;
    Ms sinceEnable = enabled ? elapsed(now, enabledSince_) : 0;

    if (clearFaultReq_) { clearFaultReq_ = false; faults_.operatorClear(); emit(Ev::Fault, 0, 0, 0, now); }
    faults_.updateDiag(raw.lifterDiag, raw.rotaryDiag, enabled, sinceEnable, now);
    faults_.updateLimits(top_.state, bottom_.state);

    TickInputs in;
    in.nowMs = now;
    in.lifterTicks = raw.lifterTicks; in.rotaryTicks = raw.rotaryTicks;
    in.topLimit = top_.state; in.bottomLimit = bottom_.state; in.homeSwitch = home_.state; in.homeSwitchRaw = raw.homeSwitch;
    in.lifterDiag = faults_.has(F_LifterDiag); in.rotaryDiag = faults_.has(F_RotaryDiag);
    in.estop = estop_.tripped();
    in.cancel = queue_.takeCancel();
    in.enabled = enabled;
    in.jogLeaseLive = queue_.jogLeaseLive(now);
    lifter_.ticks = raw.lifterTicks; rotary_.ticks = raw.rotaryTicks;

    BridgeCommand lifterOut = brakeCmd(), rotaryOut = brakeCmd();
    bool rotaryBusy = isRotaryJob(job_) || (lastOut_[1].mode == Bridge::Drive && lastOut_[1].duty != 0);

    // ---- E-STOP: latched, wins over everything ----
    if (in.estop) {
        if (!wasStopped_) {
            wasStopped_ = true;
            abortJob(JobResult::Aborted, Reason::Estop, in);
            hal_.panicStop();
            emit(Ev::Estop, static_cast<int32_t>(estop_.source()), 1, 0, now);
            if (state_ != MotionState::Stopped) { emit(Ev::StateChange, static_cast<int32_t>(state_), static_cast<int32_t>(MotionState::Stopped), 0, now); state_ = MotionState::Stopped; }
        }
        refuseAll(Reason::Estop, in);
        hal_.requestEnable(false); enableRequested_ = false;
        updateKnowledge(in, false);
        hal_.writeBridge(Axis::Lifter, lifterOut); hal_.writeBridge(Axis::Rotary, rotaryOut);
        lastOut_[0] = lifterOut; lastOut_[1] = rotaryOut;
        publish(in, lifterOut, rotaryOut);
        return;
    }
    if (wasStopped_) { wasStopped_ = false; emit(Ev::Estop, static_cast<int32_t>(estop_.source()), 0, 0, now); state_ = cfg_.commissioned ? MotionState::Idle : MotionState::Locked; emit(Ev::StateChange, static_cast<int32_t>(MotionState::Stopped), static_cast<int32_t>(state_), 0, now); idleSince_ = now; }

    updateKnowledge(in, rotaryBusy);

    // ---- driver fault recovery: EN low 50 ms, then EN high while DIAG is re-read ----
    if (faults_.takeEnCycleRequest() && enCycle_ == 0) {
        abortJob(JobResult::FaultExit, Reason::DiagFault, in);
        enCycle_ = 1; enCycleAt_ = now;
        hal_.requestEnable(false); enableRequested_ = false;
        emit(Ev::Fault, static_cast<int32_t>(faults_.mask()), 1, 1, now);
    }
    if (enCycle_ != 0) {
        if (enCycle_ == 1 && elapsed(now, enCycleAt_) >= kEnCycleLowMs) {
            enCycle_ = 2; enCycleAt_ = now;
            hal_.requestEnable(true); enableRequested_ = true; enableRequestedAt_ = now;
        } else if (enCycle_ == 2) {
            idleSince_ = now;
            bool diag = faults_.has(F_LifterDiag) || faults_.has(F_RotaryDiag);
            if (!diag) { enCycle_ = 0; emit(Ev::Fault, static_cast<int32_t>(faults_.mask()), 0, 1, now); }
            else if (elapsed(now, enCycleAt_) >= kEnVerifyMs) {
                enCycle_ = 0; hal_.requestEnable(false); enableRequested_ = false;
                emit(Ev::Fault, static_cast<int32_t>(faults_.mask()), 1, 2, now);
            }
        }
        refuseAll(Reason::DiagFault, in);
        hal_.writeBridge(Axis::Lifter, lifterOut); hal_.writeBridge(Axis::Rotary, rotaryOut);
        lastOut_[0] = lifterOut; lastOut_[1] = rotaryOut;
        publish(in, lifterOut, rotaryOut);
        return;
    }

    // ---- pre-emption: a continuous job yields to the next request ----
    if (job_ != JobKind::None && isPreemptible(job_) && queue_.size() > 0) in.cancel = true;

    // ---- program intake ----
    if (job_ == JobKind::None) {
        MotionRequest r;
        if (queue_.pop(r)) {
            stepCount_ = expandRequest(r, in, steps_); stepIdx_ = 0; programId_ = r.id; programKind_ = r.kind;
            runProgram(in);
            if (job_ != JobKind::None) { arming_ = !enabled; armingSince_ = now; }
        } else if (holdTarget_ >= 0 && cfg_.driftPct > 0 && elapsed(now, holdCheckAt_) >= 5000) {
            // ---- hold: drift correction as an idle behaviour ----
            holdCheckAt_ = now;
            Ticks tr = travel();
            int32_t driftPct = tr > 0 ? absl(in.lifterTicks - holdTarget_) * 100 / tr : 0;
            if (driftPct > cfg_.driftPct) {
                stepCount_ = 1; stepIdx_ = 0; programId_ = 0; programKind_ = JobKind::LifterHold;
                steps_[0].kind = JobKind::LifterSeek; steps_[0].a = holdTarget_; steps_[0].b = floorSpeed(1, in.lifterTicks < holdTarget_ ? 1 : -1); steps_[0].c = 0;
                emit(Ev::Hold, 1, holdTarget_, in.lifterTicks, now);
                runProgram(in);
                if (job_ != JobKind::None) { arming_ = !enabled; armingSince_ = now; }
            }
        }
    }

    // ---- job step ----
    if (job_ != JobKind::None) {
        if (!enabled) {
            if (!enableRequested_) { hal_.requestEnable(true); enableRequested_ = true; enableRequestedAt_ = now; emit(Ev::Enable, 1, 0, 0, now); }
            if (!arming_) { arming_ = true; armingSince_ = now; }
            if (elapsed(now, armingSince_) > kEnableTimeoutMs) {
                faults_.set(F_Expander);
                abortJob(JobResult::FaultExit, Reason::ExpanderFault, in);
            }
        } else {
            arming_ = false;
            stepJob(in, lifterOut, rotaryOut);
        }
    }

    // ---- enable housekeeping ----
    if (job_ == JobKind::None && enableRequested_ && elapsed(now, idleSince_) >= cfg_.idleDisableMs) {
        hal_.requestEnable(false); enableRequested_ = false; emit(Ev::Enable, 0, 0, 0, now);
    }

    // ---- output stage: a brake tick between opposite-direction drives (TB9051FTG 7.1 Note 1) ----
    BridgeCommand outs[2] = { lifterOut, rotaryOut };
    for (int i = 0; i < 2; ++i) {
        if (outs[i].mode == Bridge::Drive && outs[i].duty != 0) {
            int sign = outs[i].duty > 0 ? 1 : -1;
            if (lastDriveSign_[i] != 0 && sign != lastDriveSign_[i] && lastOut_[i].mode == Bridge::Drive && lastOut_[i].duty != 0) outs[i] = brakeCmd();
            else lastDriveSign_[i] = sign;
        }
        hal_.writeBridge(static_cast<Axis>(i), outs[i]);
        lastOut_[i] = outs[i];
    }

    // ---- state ----
    MotionState ns;
    if (!cfg_.commissioned) ns = MotionState::Locked;
    else if (faults_.globalBlocked() || faults_.has(F_LimitContradiction)) ns = MotionState::Fault;
    else if (job_ != JobKind::None) ns = MotionState::Moving;
    else ns = MotionState::Idle;
    if (ns != state_) { emit(Ev::StateChange, static_cast<int32_t>(state_), static_cast<int32_t>(ns), 0, now); state_ = ns; }

    if (elapsed(now, lastTickEvent_) >= kTickEventMs) { lastTickEvent_ = now; emit(Ev::Tick, static_cast<int32_t>(cycleUsMax_), static_cast<int32_t>(lateTicks_), 0, now); cycleUsMax_ = 0; }
    publish(in, outs[0], outs[1]);
}

void MotionSupervisor::reportTickStats(uint32_t cycleUs, bool late) { if (cycleUs > cycleUsMax_) cycleUsMax_ = cycleUs; if (late) ++lateTicks_; }

void MotionSupervisor::publish(const TickInputs& in, BridgeCommand lifterOut, BridgeCommand rotaryOut) {
    Snapshot& s = snap_;
    s.tMs = in.nowMs;
    s.state = state_;
    s.lifterTicks = lifter_.ticks;
    Ticks tr = travel();
    s.lifterTravel = tr;
    s.lifterPct = static_cast<int16_t>(tr > 0 ? clampv<int32_t>(lifter_.ticks * 100 / tr, 0, 100) : 0);
    s.lifterConf = lifter_.confidence;
    s.rotaryTicks = rotary_.ticks;
    s.ticksPerRev = cfg_.rotary.ticksPerRev;
    s.rotaryDeg = cfg_.rotary.ticksPerRev > 0 ? normalizeDeg(static_cast<int32_t>(cfg_.rotary.ticksToDeg(rotary_.ticks))) : 0;
    s.homeConf = rotary_.confidence; s.homeProv = rotary_.provenance;
    s.topLimit = in.topLimit; s.bottomLimit = in.bottomLimit; s.homeSwitch = in.homeSwitch;
    s.lifterDiag = in.lifterDiag; s.rotaryDiag = in.rotaryDiag;
    s.enabled = in.enabled;
    s.commissioned = cfg_.commissioned;
    s.rotaryDisabled = cfg_.rotaryDisabled;
    s.faults = faults_.mask();
    s.estop = in.estop; s.estopSource = static_cast<uint8_t>(estop_.source());
    Ms u = queue_.overrideUntil();
    s.overrideRemainingMs = (u != 0 && static_cast<int32_t>(u - in.nowMs) > 0) ? (u - in.nowMs) : 0;
    s.activeJob = job_; s.activeJobId = programId_;
    s.lastJob = lastJob_; s.lastResult = lastResult_; s.lastReason = static_cast<uint8_t>(lastReason_);
    s.lifterOut = lifterOut.mode == Bridge::Drive ? lifterOut.duty : 0;
    s.rotaryOut = rotaryOut.mode == Bridge::Drive ? rotaryOut.duty : 0;
    s.cycleUsMax = cycleUsMax_; s.lateTicks = lateTicks_; s.tickCount = tickCount_;
    pub_.publish(s);
}

} // namespace uppity
