#include "MotionSupervisor.h"

namespace uppity {

MotionSupervisor::MotionSupervisor(Hal& hal, EstopLatch& estop, CommandQueue& queue, SnapshotPublisher& pub, EventLog& events)
    : hal_(hal), estop_(estop), queue_(queue), pub_(pub), events_(events), prof_(&profileFor(cfg_.motor)) {
    lastOut_[0] = brakeCmd(); lastOut_[1] = brakeCmd();
}

// loop() hands settings over while the supervisor (same core, higher priority) may be reading
// them: the copy and the flag go across under the queue's lock, never half-written.
void MotionSupervisor::setConfig(const MotionConfig& c) { Lock& l = queue_.guard(); l.lock(); pendingCfg_ = c; cfgPending_ = true; l.unlock(); }

void MotionSupervisor::applyPendingConfig() {
    if (job_ != JobKind::None) return;
    MotionConfig next;
    { Lock& l = queue_.guard(); l.lock(); bool p = cfgPending_; if (p) { next = pendingCfg_; cfgPending_ = false; } l.unlock(); if (!p) return; }
    bool invertChanged = next.lifterInverted != cfg_.lifterInverted;
    cfg_ = next;
    if (invertChanged) holdTarget_ = -1;           // a hold target from the old sense would drive the wrong way
    if (invertChanged || !invertApplied_) {
        invertApplied_ = true;
        hal_.setLifterInverted(cfg_.lifterInverted);
        // The count sense just flipped: whatever was read before is meaningless. Re-reference
        // from a made switch, or admit the position is unknown.
        RawInputs raw; hal_.readInputs(raw);
        if (raw.bottomLimit && !raw.topLimit)      { hal_.setEncoder(Axis::Lifter, cfg_.bootBottomTicks); lifter_.ticks = cfg_.bootBottomTicks; lifter_.confidence = LifterConfidence::Known; }
        else if (raw.topLimit && !raw.bottomLimit) { hal_.setEncoder(Axis::Lifter, travel()); lifter_.ticks = travel(); lifter_.confidence = LifterConfidence::Known; }
        else if (invertChanged)                    { lifter_.confidence = LifterConfidence::Unknown; }
    }
    prof_ = &profileFor(cfg_.motor);
    lifter_.travelTicks = travel();
    lifter_.travelCalibrated = cfg_.travelCalibrated;
}

Ticks MotionSupervisor::travel() const { return cfg_.travelTicks > 0 ? cfg_.travelTicks : static_cast<Ticks>(prof_->defaultTravelTicks); }
// Below this height the head is inside the dome opening and must not turn. There is no safe
// guess: v3's default of half travel let Todd's head hit the lifter frame at 54 % on
// 2026-09-19. Until the builder sets it (#PSAFESPIN, or v3's stored "minheight"), only the
// top counts as clear: lifterAboveSafeNow() still passes on the top limit switch.
//
// The safe-spin height is ticks above the bottom switch, but the top switch sets the count to the
// travel in use. With only the profile's guess (no #PTRAVEL, no calibration: a factory reset and a
// settings restore, or a v3 import) the count reads ~200 high after the first trip to the top, and
// the head would turn that far below the height measured as clear (code review 2026-10, finding 7).
// So the setting counts only once the travel is measured or set; until then only the top is clear.
Ticks MotionSupervisor::safeSpin() const { return cfg_.safeSpinTicks > 0 && cfg_.travelCalibrated ? cfg_.safeSpinTicks : travel(); }
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
    if (r.lifter == LifterConfidence::Known && raw.bottomLimit && !raw.topLimit) r.lifterTicks = cfg_.bootBottomTicks;   // 0 is the switch's closing point, not the resting spot
    lifter_.confidence = r.lifter;
    lifter_.travelTicks = travel();
    if (r.lifter == LifterConfidence::Known) { hal_.setEncoder(Axis::Lifter, r.lifterTicks); lifter_.ticks = r.lifterTicks; lifter_.referencedAt = now; }
    if (r.contradiction) faults_.set(F_LimitContradiction);
    if (r.rotaryRestContact && !cfg_.rotaryDisabled) {
        hal_.setEncoder(Axis::Rotary, 0);
        rotary_.ticks = 0; rotary_.confidence = HomeConfidence::SwitchLive; rotary_.provenance = HomeProvenance::RestContact; rotary_.referencedAt = now;
    }
    top_.state = raw.topLimit; bottom_.state = raw.bottomLimit; home_.state = raw.homeSwitch;
    topWas_ = raw.topLimit; botWas_ = raw.bottomLimit; homeWas_ = raw.homeSwitch; homePending_ = raw.homeSwitch; homePendingSince_ = now;
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
    c.homeOffsetTicks = cfg_.rotary.homeOffsetTicks;
    c.rotaryDisabled = cfg_.rotaryDisabled;
    c.estop = in.estop;
    c.locked = !cfg_.commissioned && !(cfg_.commissioningActive && attendedSource(true));
    c.lifterFault = faults_.lifterBlocked();
    c.rotaryFault = faults_.rotaryBlocked();
    c.globalFault = faults_.globalBlocked();
    c.overrideLive = queue_.overrideLive(in.nowMs) && attendedSource(false);
    c.alignmentSuspect = turnedSinceHome_;
    c.clampHeld = clampHold_ && turnedSinceHome_;
    c.topLimit = in.topLimit; c.bottomLimit = in.bottomLimit;
    c.homeSwitch = in.homeSwitch && in.homeSwitchRaw;    // "closed now": the debounced value reads closed 30 ms after the head has left
    return c;
}

// The rescue override and the commissioning unlock are for someone watching the droid: the USB
// console, the web page, the commissioning flow's own steps (and the host tests). A ":PP0" from the
// dome's bus, the mesh or a trigger input never gets them (code review 2026-10, finding 5: during a
// rescue a bus command lowered the turned head into the frame). Random mode's moves pass the
// commissioning unlock (step 7 runs it; the sequencer refuses a ":PM" from the bus meanwhile) but
// never the override.
bool MotionSupervisor::attendedSource(bool forUnlock) const {
    switch (programSource_) {
        case ReqSource::SerialUsb: case ReqSource::Web: case ReqSource::Commission: case ReqSource::Test: return true;
        case ReqSource::Random: return forUnlock;
        default: return false;
    }
}

// The policy's "at or below the safe-spin height = inside the opening = lined up" is only true
// when the head came down past that height while home and has not turned since. So whenever the
// head is off home above the safe height, or turns below it (only the override allows that), it
// is marked possibly turned, and only a home at rest clears it. Bench 2026-10-03: a turned head
// clamped AT the safe height by one jog press was let down by the next press, because "at the
// threshold" counted as inside the opening, and it hit the lifter frame.
void MotionSupervisor::markAlignmentSuspect(Ms now) {
    (void)now;
    if (!cfg_.rotaryDisabled) turnedSinceHome_ = true;
}

void MotionSupervisor::updateAlignment(const TickInputs& in) {
    // Switching the rotary off does not line the head up: a head that may be turned stays so until
    // a restart (code review 2026-10, finding 6). A board that powers up with it off starts clear.
    if (!cfg_.rotaryDisabled) {
        PolicyContext c = policy(in);
        bool home = rotaryIsHome(c);
        if (!home && (lifterAboveSafe(c) || isRotaryJob(job_))) turnedSinceHome_ = true;
        else if (home && !isRotaryJob(job_)) turnedSinceHome_ = false;     // a turning head passing through home does not count
    }
    if (clampHold_ && (!turnedSinceHome_ || safeSpin() != clampFloor_ || lifter_.confidence == LifterConfidence::Unknown ||
                       in.lifterTicks > clampAt_ + kClampRearmTicks)) clampHold_ = false;
    // what the page and the log show: the flag only bites below the safe height, or at it once a creep has stopped there
    bool suspect = turnedSinceHome_ && (!lifterAboveSafeNow(in) || clampHold_);
    if (suspect != alignmentSuspect_) { alignmentSuspect_ = suspect; emit(Ev::Alignment, suspect ? 1 : 0, in.lifterTicks, !suspect && turnedSinceHome_ ? 1 : 0, in.nowMs); }
}

void MotionSupervisor::updateKnowledge(const TickInputs& in, bool rotaryBusy) {
    // Limit switches re-reference the lifter on their making edge. A making edge while the
    // opposite switch is still made is a contradiction (bench: a finger on the top switch with
    // the mast parked down; field: a short or a stuck switch) and must not move the reference.
    // FaultModel::updateLimits raises F_LimitContradiction for the duration, which blocks motion.
    if (in.topLimit && !topWas_ && in.bottomLimit) {
        emit(Ev::LimitTop, 2, in.lifterTicks, 0, in.nowMs);
    } else if (in.topLimit && !topWas_) {
        Ticks want = travel();
        if (!suppressLimitRef_) {
            if (lifter_.confidence == LifterConfidence::Known && lifter_.travelCalibrated && absl(in.lifterTicks - want) > travel() / 4) faults_.set(F_LimitMismatch);
            hal_.setEncoder(Axis::Lifter, want); lifter_.ticks = want;
        }
        lifter_.confidence = LifterConfidence::Known; lifter_.referencedAt = in.nowMs;
        emit(Ev::LimitTop, 1, in.lifterTicks, want, in.nowMs);
    } else if (!in.topLimit && topWas_) emit(Ev::LimitTop, 0, in.lifterTicks, 0, in.nowMs);
    if (in.bottomLimit && !botWas_ && in.topLimit) {
        emit(Ev::LimitBottom, 2, in.lifterTicks, 0, in.nowMs);
    } else if (in.bottomLimit && !botWas_) {
        if (!suppressLimitRef_) {
            if (lifter_.confidence == LifterConfidence::Known && lifter_.travelCalibrated && absl(in.lifterTicks) > travel() / 4) faults_.set(F_LimitMismatch);
            hal_.setEncoder(Axis::Lifter, 0); lifter_.ticks = 0;
        }
        lifter_.confidence = LifterConfidence::Known; lifter_.referencedAt = in.nowMs;
        emit(Ev::LimitBottom, 1, in.lifterTicks, 0, in.nowMs);
    } else if (!in.bottomLimit && botWas_) emit(Ev::LimitBottom, 0, in.lifterTicks, 0, in.nowMs);
    topWas_ = in.topLimit; botWas_ = in.bottomLimit;

    // While the rotary is busy every edge is logged as it happens (the home routine needs
    // them). At rest, a ball bearing parked on the edge of the contact zone flickers with bench
    // vibration (Todd, 2026-09-20: 18 toggles in 3 s with nothing moving); a state has to hold
    // for kRestSwitchHoldMs before it is logged or allowed to move the trust label.
    bool homeSeen;
    if (rotaryBusy) { homeSeen = in.homeSwitch; homePending_ = in.homeSwitch; homePendingSince_ = in.nowMs; }
    else {
        if (in.homeSwitch != homePending_) { homePending_ = in.homeSwitch; homePendingSince_ = in.nowMs; }
        homeSeen = (homePending_ != homeWas_ && elapsed(in.nowMs, homePendingSince_) >= kRestSwitchHoldMs) ? homePending_ : homeWas_;
    }
    if (homeSeen != homeWas_) emit(Ev::HomeSwitch, homeSeen ? 1 : 0, in.rotaryTicks, 0, in.nowMs);
    homeWas_ = homeSeen;

    // Rotary rest contact: a closed switch with the head at rest is a trusted zero. "At rest" is the
    // count, not the job: after an interrupted spin the job ends with the head still coasting, and
    // the switch's release debounce reads closed for 30 ms after the head has left it. A zero taken
    // then sits wherever the head happened to be (code review 2026-10, finding 1: 11-20 degrees).
    // So the switch must read closed now and the count must have been still for kRestContactStillMs.
    // A switch flagged stuck closed is not a contact at all.
    if (in.rotaryTicks != rotStillAt_) { rotStillAt_ = in.rotaryTicks; rotStillSince_ = in.nowMs; }
    bool rotaryStill = elapsed(in.nowMs, rotStillSince_) >= kRestContactStillMs;
    if (!cfg_.rotaryDisabled && !rotaryBusy && !faults_.has(F_HomeSwitchStuck)) {
        if (homeSeen && in.homeSwitch && in.homeSwitchRaw && rotaryStill) {
            if (rotary_.confidence == HomeConfidence::Latched && tpr() > 0) {
                Ticks err = signedTickDelta(0, in.rotaryTicks, tpr());
                if (absl(err) > homeToleranceTicks(cfg_.rotary.ticksPerRev)) { hal_.setEncoder(Axis::Rotary, 0); rotary_.ticks = 0; emit(Ev::HomeFrameCorrected, err, 0, 0, in.nowMs); }
            } else if (rotary_.confidence != HomeConfidence::SwitchLive) {
                hal_.setEncoder(Axis::Rotary, 0); rotary_.ticks = 0;
            }
            if (rotary_.confidence != HomeConfidence::SwitchLive) emit(Ev::ConfidenceRotary, static_cast<int32_t>(rotary_.confidence), static_cast<int32_t>(HomeConfidence::SwitchLive), 0, in.nowMs);
            rotary_.confidence = HomeConfidence::SwitchLive; rotary_.provenance = HomeProvenance::RestContact; rotary_.referencedAt = in.nowMs;
        } else if (!homeSeen && rotary_.confidence == HomeConfidence::SwitchLive) {
            rotary_.confidence = HomeConfidence::Latched;
            emit(Ev::ConfidenceRotary, static_cast<int32_t>(HomeConfidence::SwitchLive), static_cast<int32_t>(HomeConfidence::Latched), 0, in.nowMs);
        }
    }
}

// ---------------------------------------------------------------- programs

bool MotionSupervisor::lifterAboveSafeNow(const TickInputs& in) const {
    return in.topLimit || (lifter_.confidence == LifterConfidence::Known && in.lifterTicks > safeSpin());   // a STALE count is not trusted (D1)
}

// A home switch that stays closed while the head turns well past its contact arc is stuck closed
// (a stuck microswitch, a shorted lead): every resting angle would read as home and a turned head
// could be lowered (code review 2026-10, finding 3). The count's travel while the switch reads
// closed without a break is watched during every rotary job; past twice the arc plus margin (or a
// tenth of a turn, whichever is more) it is HOME_SWITCH_STUCK: the frame goes STALE, rest contacts
// and homes are refused, so a descent below the safe-spin height waits. It clears once the switch
// reads open at rest for kRestSwitchHoldMs (plan F.6 "next open read") or by #PCLEARFAULT.
// A switch stuck at power-up cannot be told from a head parked on it until the head turns.
void MotionSupervisor::watchHomeSwitch(const TickInputs& in, bool rotaryBusy) {
    if (cfg_.rotaryDisabled) { swClosedRun_ = 0; return; }
    if (!in.homeSwitchRaw) {
        swClosedRun_ = 0;
        if (!swOpen_) { swOpen_ = true; swOpenSince_ = in.nowMs; }
        if (faults_.has(F_HomeSwitchStuck) && !rotaryBusy && elapsed(in.nowMs, swOpenSince_) >= kRestSwitchHoldMs) {
            faults_.clear(F_HomeSwitchStuck); emit(Ev::Fault, static_cast<int32_t>(faults_.mask()), 0, 3, in.nowMs);
        }
        return;
    }
    swOpen_ = false;
    swClosedRun_ += in.rotaryTicks - rotLast_;
    if (!rotaryBusy || faults_.has(F_HomeSwitchStuck)) return;
    Ticks arc = cfg_.rotary.contactArcTicks;
    Ticks lim = 2 * arc + 20;
    if (tpr() > 0 && tpr() / 10 > lim) lim = tpr() / 10;
    else if (tpr() <= 0 && lim < 120) lim = 120;
    if (absl(swClosedRun_) <= lim) return;
    faults_.set(F_HomeSwitchStuck);
    emit(Ev::Fault, static_cast<int32_t>(faults_.mask()), 1, 3, in.nowMs);
    if (rotaryHomeKnown()) { emit(Ev::ConfidenceRotary, static_cast<int32_t>(rotary_.confidence), static_cast<int32_t>(HomeConfidence::Stale), 0, in.nowMs); rotary_.confidence = HomeConfidence::Stale; }
}

int MotionSupervisor::expandRequest(const MotionRequest& r, const TickInputs& in, Step* steps) {
    int n = 0;
    auto add = [&](JobKind k, int32_t a, int32_t b, int32_t c) { if (n < kMaxSteps) { steps[n].kind = k; steps[n].a = a; steps[n].b = b; steps[n].c = c; ++n; } };
    const int32_t kSkipKnown = 2, kSkipHome = 2, kSkipBelowSafe = 4, kHomeAfter = 8;
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
            // a "home if needed" request that is already home must not raise the lifter just to skip
            if (r.kind == JobKind::RotaryHome && (r.c & kSkipHome) && (rotaryIsHome(policy(in)) || cfg_.rotaryDisabled)) { add(r.kind, r.a, r.b, r.c); break; }
            if (raiseFirst) add(JobKind::LifterSeek, travel(), 0, 1);
            if (r.kind == JobKind::RotaryRelative && tpr() <= 0) add(JobKind::RotaryCharacterise, 2, 0, 0);
            if (r.kind == JobKind::RotaryCharacterise && (r.c & kHomeAfter)) {
                // the test stops just past the switch, about 11 degrees off home on Todd's rotary
                // (re-test 3, 2026-10-04): park the head where it belongs
                add(r.kind, r.a, r.b, r.c & ~kHomeAfter);
                add(JobKind::RotaryHome, 0, 0, 0);
                break;
            }
            add(r.kind, r.a, r.b, r.c);
            break;
        case JobKind::SafeLower: {
            // a head that may be turned below the safe height cannot be homed down there: raise first
            bool raise = turnedSinceHome_ && !cfg_.rotaryDisabled && !lifterAboveSafeNow(in);
            add(JobKind::LifterSeek, travel(), 0, 1 | (raise ? 0 : kSkipKnown));         // reference if unknown
            if (!cfg_.rotaryDisabled) {
                // Characterisation measures ticks/rev but parks the head wherever its last pass
                // ended (bench 2026-09-19: ~50 ticks past the switch, so the descent clamped at
                // safe height with the head out of position). Always home afterwards.
                if (tpr() <= 0) add(JobKind::RotaryCharacterise, 2, 0, kSkipHome | kSkipBelowSafe);
                add(JobKind::RotaryHome, 0, 0, kSkipHome | kSkipBelowSafe);
            }
            add(JobKind::LifterSeek, 0, r.b, 1);                                      // full travel: end on the bottom switch, not on the count
            break;
        }
        case JobKind::LifterCalibrate:
            if (!cfg_.rotaryDisabled && !rotaryHomeKnown()) {
                add(JobKind::LifterSeek, travel(), 0, 1);
                if (tpr() <= 0) add(JobKind::RotaryCharacterise, 2, 0, 0);
                add(JobKind::RotaryHome, 0, 0, 0);
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

// A descent from close above the policy floor with the head maybe turned (see kClampCreepTicks): the
// slow creep to the target (or to the floor if the target is below it), or no move at all. Returns
// as startStep does: 1 = a job started, 0 = nothing to do.
int MotionSupervisor::startFloorCreep(Ticks floor, Ticks target, bool clamped, const TickInputs& in) {
    if (in.lifterTicks - floor <= kClampRearmTicks) {
        // too close to move and stop above it: it stays, and the clamp holds as if it had come down
        flushRestReport(in);
        clampHold_ = true; clampAt_ = in.lifterTicks; clampFloor_ = safeSpin();
        restReport_ = 7; restFrom_ = in.lifterTicks; restFloor_ = floor; restSince_ = in.nowMs;
        heldAtFloor_ = true;
        emit(Ev::JobEnd, static_cast<int32_t>(JobKind::LifterSeek), static_cast<int32_t>(JobResult::Clamped), static_cast<int32_t>(Reason::RotaryNotHome), in.nowMs);
        return 0;
    }
    CreepParams p;
    p.dir = -1;
    Throttle cap = effectiveMaxThrottle(cfg_.motor, cfg_.expertMode);
    Throttle slow = cfg_.jogPowerDown > 0 ? cfg_.jogPowerDown : pctToThrottle(prof_->defaultMinPowerPct);
    p.power = clampv<Throttle>(slow, 120, cap);
    p.stallMs = prof_->stallTimeoutMs;
    p.floorTicks = floor;
    if (!clamped) p.stopAfter = in.lifterTicks - target;                         // a step to a height just above the floor
    p.brakeLeadMs = brakeLeadMs(cfg_.overshootDown, cfg_.velDown);
    p.floorLeadMs = p.brakeLeadMs > 0 ? p.brakeLeadMs : LifterSeek::kUncalibratedBrakeMs;
    jogByOverride_ = false;
    creepForSeek_ = true; creepSeekTarget_ = target;
    creep_.start(p, in.lifterTicks, in.nowMs);
    job_ = JobKind::LifterCreep;
    emit(Ev::JobStart, static_cast<int32_t>(job_), clamped ? -1 : -p.stopAfter, static_cast<int32_t>(programId_), in.nowMs);
    return 1;
}

// A rest report still waiting for the mast to settle is printed now, before a new one replaces it.
void MotionSupervisor::flushRestReport(const TickInputs& in) {
    if (!restReport_) return;
    if (restReport_ == 7) { emit(Ev::Measure, 7, in.lifterTicks, restFloor_, in.nowMs); clampAt_ = in.lifterTicks; }
    else if (restReport_ == 8) emit(Ev::Measure, 8, in.lifterTicks, restFloor_, in.nowMs);
    else emit(Ev::Measure, 6, in.lifterTicks - restFrom_, restAsked_, in.nowMs);
    restReport_ = 0;
}

int MotionSupervisor::startStep(const Step& s, const TickInputs& in) {
    PolicyContext c = policy(in);
    switch (s.kind) {
        case JobKind::LifterSeek: {
            if ((s.c & 2) && lifter_.confidence == LifterConfidence::Known) return 0;    // skip: already referenced (a STALE count is re-referenced at the top)
            Ticks target = clampv<Ticks>(s.a, 0, travel());
            if (programKind_ == JobKind::SafeLower && target == 0) emit(Ev::LightKit, 1, 0, 0, in.nowMs);   // lights off before lowering
            Decision d = checkLifterSeek(c, in.lifterTicks, target);
            if (!d.ok()) { refuse(s, d.reason, programId_, in); return -1; }
            bool clamped = d.verdict == Verdict::Clamp;
            if (clamped) { emit(Ev::SeekClamped, target, d.value, static_cast<int32_t>(d.reason), in.nowMs); target = d.value; }
            int dir = target >= in.lifterTicks ? 1 : -1;
            SeekParams p;
            p.target = target;
            p.speed = floorSpeed(static_cast<Throttle>(s.b), dir);
            p.minPower = floorSpeed(1, dir);
            p.approachPower = cfg_.approachPower > 0 ? cfg_.approachPower : pctToThrottle(prof_->defaultSeekBottomPowerPct);
            p.fullTravel = (s.c & 1) != 0 && !clamped;
            p.brake = dir > 0 ? makeBrakeTable(cfg_.overshootUp, cfg_.velUp) : makeBrakeTable(cfg_.overshootDown, cfg_.velDown);
            // The policy's floor for a descent with the head maybe turned (the same one a jog stops
            // at), whether the target was clamped to it or lies above it: a target just above the
            // safe-spin height must not be overshot below it (review of Part A, 2026-10-04).
            if (dir < 0) { Decision f = checkLifterCreep(c, -1); if (f.ok() && f.value != kNoFloor) p.floorTicks = f.value; }
            p.prof = prof_;
            if (!p.fullTravel && absl(target - in.lifterTicks) < 5) { lastReason_ = Reason::AlreadyThere; emit(Ev::JobEnd, static_cast<int32_t>(s.kind), static_cast<int32_t>(JobResult::Done), static_cast<int32_t>(Reason::AlreadyThere), in.nowMs); return 0; }
            if (p.fullTravel && ((target == 0 && in.bottomLimit) || (target == travel() && in.topLimit))) { lastReason_ = Reason::AlreadyThere; emit(Ev::JobEnd, static_cast<int32_t>(s.kind), static_cast<int32_t>(JobResult::Done), static_cast<int32_t>(Reason::AlreadyThere), in.nowMs); return 0; }
            if (p.floorTicks != kNoFloor && in.lifterTicks - p.floorTicks <= kClampCreepTicks)
                return startFloorCreep(p.floorTicks, target, clamped, in);                  // close above the floor: the slow way, or not at all
            // only the override lets an unaligned head come down into the opening: remember that it did
            if (dir < 0 && target <= safeSpin() && c.overrideLive && !rotaryIsHome(c)) markAlignmentSuspect(in.nowMs);
            seekClamped_ = clamped;
            seek_.start(p, in.lifterTicks, in.nowMs);
            job_ = JobKind::LifterSeek;
            emit(Ev::JobStart, static_cast<int32_t>(job_), target, static_cast<int32_t>(programId_), in.nowMs);
            return 1;
        }
        case JobKind::LifterCreep: {
            int dir = s.a < 0 ? -1 : 1;
            bool step = (s.c & 2) != 0;
            Decision d = checkLifterCreep(c, dir);
            if (!d.ok()) { refuse(s, d.reason, programId_, in); return -1; }
            CreepParams p;
            p.dir = dir;
            p.jog = (s.c & 1) != 0 && !step;
            Throttle cap = effectiveMaxThrottle(cfg_.motor, cfg_.expertMode);
            Throttle profMin = pctToThrottle(prof_->defaultMinPowerPct);
            Throttle req = static_cast<Throttle>(s.b);
            if (p.jog || step) {
                // jog and #PNUDGE default to the slowest power that is known to move this lifter
                // from rest (the calibrated minimum): continuous, never pulsed, so it creeps
                Throttle slow = dir > 0 ? cfg_.jogPowerUp : cfg_.jogPowerDown;
                if (slow <= 0) slow = profMin;
                p.power = clampv<Throttle>(req > 0 ? req : slow, 120, cap);
            } else p.power = clampv<Throttle>(req > 0 ? req : profMin, profMin, cap);
            p.stallMs = prof_->stallTimeoutMs;       // the profile's figure (700 ms on a 19:1), as the calibration's creeps use
            if (step) p.stopAfter = clampv<Ticks>(absl(s.a), 1, kMaxStepTicks);
            p.floorTicks = d.value;
            p.brakeLeadMs = dir < 0 ? brakeLeadMs(cfg_.overshootDown, cfg_.velDown) : brakeLeadMs(cfg_.overshootUp, cfg_.velUp);
            p.floorLeadMs = p.brakeLeadMs > 0 ? p.brakeLeadMs : LifterSeek::kUncalibratedBrakeMs;   // no table: a creep stopped ON the floor ran 6-33 below it (simulator)
            { PolicyContext without = c; without.overrideLive = false; Decision w = checkLifterCreep(without, dir);
              jogByOverride_ = p.jog && c.overrideLive && (!w.ok() || w.value > d.value); }
            if (dir < 0 && c.overrideLive && !rotaryIsHome(c) && (!step || in.lifterTicks - p.stopAfter <= safeSpin())) markAlignmentSuspect(in.nowMs);
            creepForSeek_ = false;
            creep_.start(p, in.lifterTicks, in.nowMs);
            job_ = JobKind::LifterCreep;
            emit(Ev::JobStart, static_cast<int32_t>(job_), step ? dir * p.stopAfter : dir, static_cast<int32_t>(programId_), in.nowMs);
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
                int16_t curDeg = normalizeDeg(static_cast<int32_t>(cfg_.rotary.ticksToDeg(in.rotaryTicks - cfg_.rotary.homeOffsetTicks)));
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
            p.stepBelow = clampv<Ticks>(tpr() * 6 / 360, 4, 1 << 20);                 // ":PD2"-sized moves walk in single ticks
            { Ticks dist = absl(target - in.rotaryTicks); if (dist <= p.stepBelow) p.fudge = 0; }
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
            jogByOverride_ = jog && !checkRotary(c, false).ok();      // turning below the safe height: only while the override lasts
            if (jog && !lifterAboveSafe(c)) markAlignmentSuspect(in.nowMs);    // turning inside the opening: only the override allowed it
            RotarySpinParams p;
            p.rc = &cfg_.rotary; p.jog = jog;
            if (jog) p.stallMs = 800;             // a slow jog takes longer to break loose than a spin; still stops if the head is held
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
            // "skip if home": the head must actually be within tolerance, not merely referenced.
            // (Bench 2026-09-19: after characterisation the frame was Latched but the head sat
            // ~70 ticks past the switch; skipping here left :PH clamped at safe height.)
            if ((s.c & 2) && (rotaryIsHome(c) || cfg_.rotaryDisabled)) return 0;
            if ((s.c & 4) && !turnedSinceHome_ && lifter_.confidence == LifterConfidence::Known && in.lifterTicks <= safeSpin() && !in.topLimit) return 0;   // inside the opening, lined up: unless it may be turned
            Decision d = checkRotary(c, false);
            if (!d.ok()) { refuse(s, d.reason, programId_, in); return -1; }
            if (faults_.has(F_HomeSwitchStuck)) { refuse(s, Reason::SwitchStuck, programId_, in); return -1; }   // its sweep would take the stuck reading as an edge
            RotaryHomeParams p;
            p.rc = &cfg_.rotary; p.floor = rotaryFloor(); p.referenced = rotaryReferenced(); p.tpr = tpr(); p.sweepDir = 1; p.homeOffset = cfg_.rotary.homeOffsetTicks;
            rhome_.start(p, in.rotaryTicks, in.nowMs);
            job_ = JobKind::RotaryHome;
            emit(Ev::JobStart, static_cast<int32_t>(job_), p.referenced ? 1 : 0, static_cast<int32_t>(programId_), in.nowMs);
            return 1;
        }
        case JobKind::RotaryCharacterise: {
            if ((s.c & 2) && (rotaryHomeKnown() || cfg_.rotaryDisabled)) return 0;
            if ((s.c & 4) && !turnedSinceHome_ && lifter_.confidence == LifterConfidence::Known && in.lifterTicks <= safeSpin() && !in.topLimit) return 0;   // inside the opening, lined up: unless it may be turned
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
    if (job_ == JobKind::None && stepCount_ > 0) {
        // a descent held where it was, next to the safe-spin height with the head maybe turned, ends as clamped
        if (heldAtFloor_) { heldAtFloor_ = false; endProgram(StepOutcome::done(JobResult::Clamped, Reason::RotaryNotHome), in); return; }
        endProgram(StepOutcome::done(JobResult::Done, lastReason_ == Reason::AlreadyThere ? Reason::AlreadyThere : Reason::None), in);
    }
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
    // a nudge, or a creep the safe-spin floor stopped: say where it comes to rest (see the tick)
    if (k == JobKind::LifterCreep && (o.jobResult == JobResult::Clamped || creep_.stopAfter() > 0)) {
        flushRestReport(in);
        restReport_ = o.jobResult == JobResult::Clamped ? 7 : creepForSeek_ ? 8 : 6;
        restFrom_ = creep_.startTicks(); restFloor_ = o.jobResult == JobResult::Clamped || !creepForSeek_ ? creep_.floorTicks() : creepSeekTarget_;
        restAsked_ = creep_.dir() * creep_.stopAfter(); restSince_ = in.nowMs;
        if (o.jobResult == JobResult::Clamped) { clampHold_ = true; clampAt_ = in.lifterTicks; clampFloor_ = safeSpin(); }
    }
    // A seek to a height (not to a switch): say where it comes to rest. One the safe-spin floor
    // clamped or stopped holds the clamp as a creep's does: a second descent from rest just above
    // the floor starts too slowly for the speed estimate to brake it in time (Phase 5 Part A).
    if (k == JobKind::LifterSeek && o.jobResult == JobResult::DoneTarget) {
        flushRestReport(in);
        bool atFloor = seekClamped_ || seek_.stoppedByFloor();
        restReport_ = atFloor ? 7 : 8;
        restFrom_ = seek_.startTicks(); restFloor_ = atFloor ? seek_.floorTicks() : seek_.target(); restSince_ = in.nowMs;
        if (atFloor) { clampHold_ = true; clampAt_ = in.lifterTicks; clampFloor_ = safeSpin(); }
    }
    if (k == JobKind::LifterCalibrate) {
        suppressLimitRef_ = false;
        if (o.result == StepResult::Done) {
            lastCal_ = calib_.result();
            cfg_.travelTicks = lastCal_.travel; cfg_.travelCalibrated = true;
            cfg_.minPowerUp = lastCal_.minUp; cfg_.minPowerDown = lastCal_.minDown;
            cfg_.bootBottomTicks = -lastCal_.bottomRest;
            for (int i = 0; i < kSlots; ++i) { cfg_.overshootUp[i] = lastCal_.overUp[i]; cfg_.overshootDown[i] = lastCal_.overDown[i]; cfg_.velUp[i] = lastCal_.velUp[i]; cfg_.velDown[i] = lastCal_.velDown[i]; }
            { Lock& l = queue_.guard(); l.lock(); if (cfgPending_) { pendingCfg_.travelTicks = cfg_.travelTicks; pendingCfg_.travelCalibrated = true; pendingCfg_.minPowerUp = cfg_.minPowerUp; pendingCfg_.minPowerDown = cfg_.minPowerDown; pendingCfg_.bootBottomTicks = cfg_.bootBottomTicks; } l.unlock(); }
            lifter_.travelTicks = cfg_.travelTicks; lifter_.travelCalibrated = true;
            calUpdated_ = true;
            emit(Ev::Measure, 3, lastCal_.travel, static_cast<int32_t>(lastCal_.travelSpreadPct * 10), in.nowMs);
            emit(Ev::Measure, 4, lastCal_.minUp, lastCal_.minDown, in.nowMs);
        }
        // The machine ends at the bottom limit: re-reference there, with 0 where the switch closed
        // (as every other bottom reference does), not at the resting spot past it.
        if (in.bottomLimit) {
            const CalibrateResult& cr = calib_.result();
            Ticks at = cr.bottomMakeValid ? in.lifterTicks - cr.bottomMake : 0;
            hal_.setEncoder(Axis::Lifter, at); lifter_.ticks = at; lifter_.confidence = LifterConfidence::Known;
        }
    }
    if (isRotaryJob(k)) {
        // A job that started at rest on the switch is still SWITCH_LIVE here (the label does not
        // follow the switch while the rotary is busy): a stall demotes it as well, or the frozen
        // count would be trusted as home (code review 2026-10, finding 2).
        if (o.jobResult == JobResult::Stalled) { faults_.set(F_RotaryStall); if (rotaryHomeKnown()) { emit(Ev::ConfidenceRotary, static_cast<int32_t>(rotary_.confidence), static_cast<int32_t>(HomeConfidence::Stale), 0, in.nowMs); rotary_.confidence = HomeConfidence::Stale; } }
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
            emit(Ev::HomeSettle, lastHome_.contact ? 1 : 0, lastHome_.wiggles | (static_cast<int32_t>(lastHome_.lastMoveResult) << 8) | (static_cast<int32_t>(lastHome_.lastMoveReason) << 16), lastHome_.nudges, in.nowMs);
            if (was != HomeConfidence::Latched) emit(Ev::ConfidenceRotary, static_cast<int32_t>(was), static_cast<int32_t>(HomeConfidence::Latched), 0, in.nowMs);
        } else if (rotaryHomeKnown()) {
            emit(Ev::ConfidenceRotary, static_cast<int32_t>(rotary_.confidence), static_cast<int32_t>(HomeConfidence::Stale), 0, in.nowMs);
            rotary_.confidence = HomeConfidence::Stale;   // frame untouched, trust reduced
        }
    }
    if (k == JobKind::RotaryCharacterise && o.result == StepResult::Done) {
        lastChar_ = rchar_.result();
        emit(Ev::Measure, 1, static_cast<int32_t>(lastChar_.meanTpr * 10.f + 0.5f), static_cast<int32_t>(lastChar_.spreadPct * 10.f + 0.5f), in.nowMs);   // rounded, as "ROTARY stored" prints it
        emit(Ev::Measure, 2, lastChar_.arcMean, lastChar_.closures, in.nowMs);
        emit(Ev::Measure, 5, lastChar_.confident ? 2 : lastChar_.probable ? 1 : 0, lastChar_.creepFits ? 1 : 0, in.nowMs);
        if (lastChar_.confident || lastChar_.probable) {
            cfg_.rotary.ticksPerRev = lastChar_.meanTpr;
            cfg_.rotary.confident = lastChar_.confident;
            cfg_.rotary.contactArcTicks = static_cast<uint16_t>(lastChar_.arcMean);
            cfg_.rotary.chatterPerPass = static_cast<uint8_t>(lastChar_.detections > 0 ? clampv<int>(lastChar_.closures / lastChar_.detections, 0, 255) : 0);
            charUpdated_ = true;
            { Lock& l = queue_.guard(); l.lock(); if (cfgPending_) pendingCfg_.rotary = cfg_.rotary; l.unlock(); }
        }
        Ticks zero = lastChar_.lastZero;
        hal_.setEncoder(Axis::Rotary, in.rotaryTicks - zero);
        rotary_.ticks = in.rotaryTicks - zero;
        rotary_.confidence = HomeConfidence::Latched; rotary_.provenance = HomeProvenance::EdgeCentre; rotary_.referencedAt = in.nowMs;
        rotary_.lastEdgeArcTicks = lastChar_.arcMean;
    }

    if (o.result != StepResult::Done) restRequired_ = true;   // interrupted: the next program waits for rest
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
    } else {
        lastJob_ = programKind_; lastResult_ = o.jobResult; lastReason_ = o.reason;
        // The composite reports its own outcome: its steps may all have been skipped (":PH"
        // from the bottom prints nothing otherwise) or the failure belongs to the whole
        // program ("SAFE_LOWER FAILED HOME_FAILED"), not to the step that tripped it.
        if (programKind_ == JobKind::SafeLower) emit(Ev::JobEnd, static_cast<int32_t>(programKind_), static_cast<int32_t>(o.jobResult), static_cast<int32_t>(o.reason), in.nowMs);
    }
    if (programId_ != 0) lastProgramId_ = programId_;
    stepCount_ = 0; stepIdx_ = 0; programKind_ = JobKind::None; programId_ = 0; programSource_ = ReqSource::None;
    heldAtFloor_ = false;
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
        if (r.id != 0) lastProgramId_ = r.id;
    }
}

// ---------------------------------------------------------------- the tick

void MotionSupervisor::tick(Ms now) {
    ++tickCount_;
    queue_.expireLeases(now);                       // an expired override or jog lease is cleared, never left to wrap
    applyPendingConfig();

    RawInputs raw; hal_.readInputs(raw);

    // Rescue "set rotary home here": the operator vouches that the head is lined up right now.
    if (setHomeReq_.exchange(false, std::memory_order_acq_rel)) {
        Reason why = estop_.tripped() ? Reason::Estop : cfg_.rotaryDisabled ? Reason::RotaryDisabled : job_ != JobKind::None ? Reason::Busy : Reason::None;
        emit(Ev::HomeManual, raw.rotaryTicks, static_cast<int32_t>(rotary_.confidence), static_cast<int32_t>(why), now);
        if (why == Reason::None) {
            Ticks at = cfg_.rotary.homeOffsetTicks;          // the visual home is where "0 degrees" sits
            hal_.setEncoder(Axis::Rotary, at); raw.rotaryTicks = at; rotary_.ticks = at; rotLast_ = at;   // a re-reference, not travel (watchHomeSwitch)
            rotary_.confidence = HomeConfidence::Latched; rotary_.provenance = HomeProvenance::Manual; rotary_.referencedAt = now;
            faults_.clear(F_RotaryStall);
        }
    }

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
    if (queue_.takeCancelAll()) refuseAll(Reason::Cancel, in);   // Stop: what was queued ends too (D2)
    in.enabled = enabled;
    in.jogLeaseLive = queue_.jogLeaseLive(now);
    lifter_.ticks = raw.lifterTicks; rotary_.ticks = raw.rotaryTicks;
    if (in.lifterTicks == stillL_ && in.rotaryTicks == stillR_) { if (stillTicks_ < 65535) ++stillTicks_; }
    else { stillTicks_ = 0; stillL_ = in.lifterTicks; stillR_ = in.rotaryTicks; }
    if (restReport_ && (stillTicks_ >= kEstopStillTicks || elapsed(now, restSince_) >= 1000 || job_ != JobKind::None)) flushRestReport(in);

    BridgeCommand lifterOut = brakeCmd(), rotaryOut = brakeCmd();
    bool rotaryBusy = isRotaryJob(job_) || (lastOut_[1].mode == Bridge::Drive && lastOut_[1].duty != 0);

    // ---- E-STOP: latched, wins over everything ----
    if (in.estop) {
        if (!wasStopped_) {
            wasStopped_ = true;
            abortJob(JobResult::Aborted, Reason::Estop, in);
            hal_.panicStop();
            queue_.setOverride(0);                 // a stop ends the rescue override and any jog lease: both must be asked for again
            queue_.jogRelease(now);
            holdTarget_ = -1;                      // nothing resumes after the release, the drift hold included (plan F.5)
            emit(Ev::Estop, static_cast<int32_t>(estop_.source()), 1, 0, now);
            if (state_ != MotionState::Stopped) { emit(Ev::StateChange, static_cast<int32_t>(state_), static_cast<int32_t>(MotionState::Stopped), 0, now); state_ = MotionState::Stopped; }
            estopBraking_ = enableRequested_ && enabled;          // drivers on: keep them on to brake (see kEstopStillTicks)
            estopAt_ = now; estopL_ = in.lifterTicks; estopR_ = in.rotaryTicks;
        }
        refuseAll(Reason::Estop, in);
        if (estopBraking_) {
            bool still = stillTicks_ >= kEstopStillTicks;
            if (still || !enabled || elapsed(now, estopAt_) >= kEstopBrakeMaxMs) {
                estopBraking_ = false;
                int32_t ms = static_cast<int32_t>(elapsed(now, estopAt_));
                emit(Ev::EstopSettled, in.lifterTicks - estopL_, in.rotaryTicks - estopR_, (ms > 0xffff ? 0xffff : ms) | (still ? 0 : 0x10000), now);
            }
        }
        if (!estopBraking_) { hal_.requestEnable(false); enableRequested_ = false; }
        updateKnowledge(in, false);
        hal_.writeBridge(Axis::Lifter, lifterOut); hal_.writeBridge(Axis::Rotary, rotaryOut);
        lastOut_[0] = lifterOut; lastOut_[1] = rotaryOut;
        publish(in, lifterOut, rotaryOut);
        return;
    }
    if (wasStopped_) { wasStopped_ = false; estopBraking_ = false; emit(Ev::Estop, static_cast<int32_t>(estop_.source()), 0, 0, now); state_ = cfg_.commissioned ? MotionState::Idle : MotionState::Locked; emit(Ev::StateChange, static_cast<int32_t>(MotionState::Stopped), static_cast<int32_t>(state_), 0, now); idleSince_ = now; }

    updateKnowledge(in, rotaryBusy);
    watchHomeSwitch(in, rotaryBusy);

    // ---- alignment: may the head be turned? (see markAlignmentSuspect) ----
    updateAlignment(in);

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

    // ---- pre-emption: a continuous job yields to the next request; a seek or rotation is interrupted by it ----
    if (job_ != JobKind::None && (isPreemptible(job_) || isRetargetable(job_)) && queue_.size() > 0) in.cancel = true;

    // ---- program intake ----
    if (job_ == JobKind::None) {
        MotionRequest r;
        bool resting = restRequired_ && stillTicks_ < kRestStillTicks && elapsed(now, idleSince_) < kRestMaxMs;
        if (holdIntake() || resting) { /* NVS commit in progress, or the mechanism is still coasting: leave requests queued */ }
        else if (queue_.pop(r)) {
            restRequired_ = false;
            programSource_ = r.source;
            stepCount_ = expandRequest(r, in, steps_); stepIdx_ = 0; programId_ = r.id; programKind_ = r.kind;
            runProgram(in);
            if (job_ != JobKind::None) { arming_ = !enabled; armingSince_ = now; }
        } else if (!resting && holdTarget_ >= 0 && cfg_.driftPct > 0 && elapsed(now, holdCheckAt_) >= 5000) {
            // ---- hold: drift correction as an idle behaviour ----
            holdCheckAt_ = now;
            Ticks tr = travel();
            int32_t driftPct = tr > 0 ? absl(in.lifterTicks - holdTarget_) * 100 / tr : 0;
            if (driftPct > cfg_.driftPct) {
                stepCount_ = 1; stepIdx_ = 0; programId_ = 0; programKind_ = JobKind::LifterHold; programSource_ = ReqSource::None;
                steps_[0].kind = JobKind::LifterSeek; steps_[0].a = holdTarget_; steps_[0].b = floorSpeed(1, in.lifterTicks < holdTarget_ ? 1 : -1); steps_[0].c = 0;
                emit(Ev::Hold, 1, holdTarget_, in.lifterTicks, now);
                runProgram(in);
                if (job_ != JobKind::None) { arming_ = !enabled; armingSince_ = now; }
            }
        }
    }

    // ---- a jog the override allowed ends with the override (D4) ----
    // The policy is asked when a step starts. A held jog is the one job that can outlast the
    // reason it was allowed: Rescue's 60 s override running out, or "End now".
    if (jogByOverride_ && (job_ == JobKind::RotaryJog || job_ == JobKind::LifterCreep) && !(queue_.overrideLive(now) && attendedSource(false)))
        abortJob(JobResult::Aborted, Reason::PolicyChange, in);
    if (job_ == JobKind::None) jogByOverride_ = false;

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
        // An E-STOP from another core can land after this tick sampled the latch (web, port 81, UART):
        // its panic hook has zeroed the outputs, and a drive written now would undo that for a
        // millisecond. The latch is read again right before each write (plan §3.2).
        if (estop_.tripped()) outs[i] = brakeCmd();
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
    rotLast_ = rotary_.ticks;                       // every tick ends here: the count in the frame the next tick reads
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
    s.rotaryDeg = cfg_.rotary.ticksPerRev > 0 ? normalizeDeg(static_cast<int32_t>(cfg_.rotary.ticksToDeg(rotary_.ticks - cfg_.rotary.homeOffsetTicks))) : 0;
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
    bool ovr = s.overrideRemainingMs > 0;                     // one log line when it starts and one when it ends, however it ends
    if (ovr != overrideWas_) { overrideWas_ = ovr; emit(Ev::Override, ovr ? 1 : 0, static_cast<int32_t>(s.overrideRemainingMs), 0, in.nowMs); }
    s.alignmentSuspect = alignmentSuspect_;
    s.commissioning = cfg_.commissioningActive;
    s.activeJob = job_; s.activeJobId = programId_; s.lastProgramId = lastProgramId_;
    s.activeProgram = job_ != JobKind::None ? programKind_ : JobKind::None;
    s.jobPhase = 0; s.jobDetail = 0;
    switch (job_) {
        case JobKind::LifterSeek: s.jobPhase = static_cast<uint8_t>(seek_.phase()); break;
        case JobKind::RotaryHome: s.jobPhase = static_cast<uint8_t>(rhome_.phase()); break;
        case JobKind::LifterCalibrate: s.jobPhase = static_cast<uint8_t>(calib_.phase()); s.jobDetail = calib_.progress(); break;
        case JobKind::RotaryCharacterise: s.jobDetail = static_cast<int16_t>(rchar_.detections()); break;
        case JobKind::LifterCreep: s.jobDetail = static_cast<int16_t>(creep_.stopAfter() > 0 ? creep_.dir() * creep_.stopAfter() : creep_.dir()); break;
        default: break;
    }
    s.lastJob = lastJob_; s.lastResult = lastResult_; s.lastReason = static_cast<uint8_t>(lastReason_);
    s.lifterOut = lifterOut.mode == Bridge::Drive ? lifterOut.duty : 0;
    s.rotaryOut = rotaryOut.mode == Bridge::Drive ? rotaryOut.duty : 0;
    s.cycleUsMax = cycleUsMax_; s.lateTicks = lateTicks_; s.tickCount = tickCount_;
    pub_.publish(s);
}

} // namespace uppity
