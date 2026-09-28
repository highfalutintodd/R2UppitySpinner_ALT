#pragma once
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "Units.h"
#include "Settings.h"
#include "Sequencer.h"
#include "CommandQueue.h"
#include "StatusSnapshot.h"
#include "EventLog.h"
#include "EventFormat.h"
#include "FaultModel.h"
#include "Parser.h"
#include "TextSink.h"
// Commissioning (docs/REWRITE-PLAN.md §4, docs/PHASE4-BRIEF.md): the guided first setup that
// takes a board from LOCKED to commissioned = 2. Pure; the console is its Host on the board.
//
// The step lives in Settings.commissionStep (persisted), so a page reload or a power cycle
// resumes where it stopped. While the flow is active it lifts the lock for the motions its
// steps need (MotionConfig.commissioningActive); an E-STOP pauses it. Every pass/fail is decided
// here from what the supervisor reports (program results, measurements), never on the page's say.
//
//   1 Profile     the lifter motor profile and five acknowledgements (the page's checklist)
//   2 Wiring      the page shows the live switches; passes with no contradiction and no driver fault
//   3 Creep       the first motion: a continuous creep UP to the top switch. A reversed mechanism shows here
//                 (the bottom switch closes, or it stalls on its bottom stop). Deviation from the plan's "down
//                 then up": with the rotary not yet homed the policy rightly refuses a descent from the top,
//                 so the way down is proven by step 5, whose first move is a creep down to the bottom switch
//   4 Rotary      at the top: three-turn characterisation, then a home (nothing to do with the rotary disabled)
//   5 Calibrate   the lifter calibration (#PSC): bottom, top, bottom, top, bottom by continuous creeps, then the tables
//   6 SafeSpin    MEASURED, never assumed (bench 2026-09-19: a half-travel guess hit the frame). RUN clears the
//                 old height (only the top counts), raises to the top and turns the head to 90 degrees; the
//                 operator lowers in small #PNUDGE steps under the override, watching; #PSAFESPINHERE stores
//                 that height plus a margin
//   7 Acceptance  home, 0 %, 50 %, 100 %, home, then one gentle random action
//   8 Complete    commissioned = 2
namespace uppity {

class CommissionFlow {
  public:
    enum Step : uint8_t { NotStarted = 0, Profile, Wiring, Creep, Rotary, Calibrate, SafeSpin, Acceptance, Complete };
    static constexpr int kSteps = 7;
    enum class Run : uint8_t { Idle, Busy, Passed, Failed };
    static constexpr int kTurnDeg = 90;               // the head's angle for the safe-spin measurement
    static constexpr int kTurnedDeg = 45;             // #PSAFESPINHERE wants the head at least this far from home
    static constexpr Ms  kCalStoreMs = 3000;          // a finished calibration must reach the settings within this

    struct Host {
        virtual ~Host() = default;
        virtual Settings& settings() = 0;
        virtual const Calibration& calibration() = 0;
        virtual const CharacteriseResult& characterisation() = 0;
        virtual void settingsChanged() = 0;           // re-apply to the supervisor (incl. the active flag) and persist
        virtual uint32_t pushJob(const MotionRequest& r) = 0;   // 0 = queue full
        virtual bool runChain(const Command* els, int n) = 0;   // through the sequencer
        virtual bool chainActive() = 0;
        virtual void cancelMotion() = 0;              // chain cleared, running job cancelled, random off
        virtual void randomStart(int level) = 0;
        virtual void randomStop() = 0;
        virtual uint32_t randomActions() = 0;
        virtual bool expanderOk() = 0;
    };

    void begin(Host& h, TextSink& out) { h_ = &h; out_ = &out; }

    bool active() const { return active_; }
    uint8_t step() const { return h_ ? h_->settings().commissionStep : 0; }
    Run run() const { return run_; }
    const char* message() const { return msg_; }
    int32_t detail() const { return detail_; }
    uint16_t previousSafeSpin() const { return prevSafeSpin_; }
    static const char* stepName(int s) {
        static const char* k[] = { "not started", "profile", "wiring check", "lift test", "rotary", "calibration", "safe-spin height", "acceptance", "complete" };
        return s >= 0 && s <= Complete ? k[s] : "?";
    }
    static const char* runName(Run r) { return r == Run::Busy ? "busy" : r == Run::Passed ? "passed" : r == Run::Failed ? "failed" : "idle"; }

    // ---------------------------------------------------------------- commands

    // #PCOMMISSIONSTART: start, or resume at the stored step.
    bool start(const Snapshot& s) {
        Settings& st = h_->settings();
        if (s.estop) { out_->line("COMMISSION=refused (E-STOP latched: reset it first)"); return false; }
        if (st.commissionStep == Complete) { out_->line("COMMISSION=already complete (#PCOMMISSIONRESET runs it again from step 1)"); return false; }
        if (st.commissionStep == NotStarted) { st.commissionStep = Profile; clearRun(); }
        active_ = true;
        h_->settingsChanged();
        out_->printf("COMMISSION=active step %d/%d (%s): the commissioning steps may move the motors even while LOCKED", st.commissionStep, kSteps, stepName(st.commissionStep));
        return true;
    }
    // #PCOMMISSIONSTOP, or an E-STOP: the step is kept for a resume.
    void stop(const char* why) {
        if (!active_ && run_ != Run::Busy) return;
        bool wasBusy = run_ == Run::Busy;
        if (wasBusy) { h_->cancelMotion(); fail("stopped (%s)", why); }
        if (randomOurs_) { h_->randomStop(); randomOurs_ = false; }
        active_ = false;
        h_->settingsChanged();
        out_->printf("COMMISSION=paused at step %d (%s): #PCOMMISSIONSTART resumes", step(), why);
    }
    // #PCOMMISSIONRESET: back to step 1. The board keeps whatever "commissioned" it had until the flow completes again.
    void reset() {
        if (run_ == Run::Busy) { h_->cancelMotion(); if (randomOurs_) { h_->randomStop(); randomOurs_ = false; } }
        h_->settings().commissionStep = active_ ? Profile : NotStarted;
        clearRun();
        h_->settingsChanged();
        out_->printf("COMMISSION=reset to step %d", step());
    }
    // #PCOMMISSIONBYPASS: the expert bypass (the same as #PUNLOCK). The one bypass lives on the commissioning page.
    void bypass() {
        Settings& st = h_->settings();
        st.commissioned = 1;
        active_ = false;
        h_->settingsChanged();
        out_->line("COMMISSIONED=1 (expert bypass: unlocked without commissioning; saved; #PLOCK to undo)");
    }

    // #PCOMMISSIONRUN: start the current step's action.
    bool runStep(const Snapshot& s, Ms now) {
        if (!active_) { out_->line("COMMISSION=not active (#PCOMMISSIONSTART)"); return false; }
        if (run_ == Run::Busy) { out_->line("COMMISSION=busy (the step is running)"); return false; }
        if (s.estop) { out_->line("COMMISSION=refused (E-STOP latched)"); return false; }
        if (s.activeJob != JobKind::None || h_->chainActive()) { out_->line("COMMISSION=wait (something is still moving)"); return false; }
        Settings& st = h_->settings();
        clearRun();
        startedAt_ = now;
        switch (st.commissionStep) {
            case Creep:
                if (s.topLimit) { pass("the mast is already on the top switch: step 5's first move (a creep down) checks the direction"); return true; }
                upFromBottom_ = s.bottomLimit && s.lifterConf == LifterConfidence::Known;
                return pushOrFail(creepRequest(+1), StageCreep, "creep up to the top switch (continuous, the profile's lowest power)");
            case Rotary:
                if (st.rotaryDisabled) { pass("no rotary fitted (#PROTARYOFF1): nothing to measure"); return true; }
                { MotionRequest r; r.kind = JobKind::RotaryCharacterise; r.a = 3; return pushOrFail(r, StageRotChar, "raising to the top, then three turns to measure the rotary"); }
            case Calibrate: {
                calAtStart_ = h_->calibration().measuredAtMs; calValidAtStart_ = h_->calibration().valid != 0;
                // the calibration descends, so it refuses a head that is off home: home first when needed
                // (queued ahead of it; a home is never interrupted by the request behind it)
                MotionRequest home; home.kind = JobKind::RotaryHome; home.c = 2; home.source = ReqSource::Commission;
                if (h_->pushJob(home) == 0) { fail("the command queue is full: try again"); return false; }
                MotionRequest r; r.kind = JobKind::LifterCalibrate;
                return pushOrFail(r, StageCal, "calibrating the lifter (several minutes; E-STOP stops it)");
            }
            case SafeSpin: {
                if (st.rotaryDisabled) { pass("no rotary fitted: the head never turns, so no safe-spin height is needed"); return true; }
                if (st.safeSpinTicks) prevSafeSpin_ = st.safeSpinTicks;
                st.safeSpinTicks = 0;                  // only the top counts until the measured height is stored
                h_->settingsChanged();
                out_->printf("SAFESPIN=0 (commissioning: only the top counts as clear until the measured height is stored; it was %u)", prevSafeSpin_);
                MotionRequest r; r.kind = JobKind::LifterSeek; r.a = s.lifterTravel; r.c = 1;   // to the top switch first, then turn
                return pushOrFail(r, StageSpinRaise, "raising to the top");
            }
            case Acceptance: {
                static const char* kChain = ":PA0:PP0:PP50:PP100:PA0";
                Command els[Sequencer::kMaxElements];
                int n = Sequencer::parseChain(kChain, els, Sequencer::kMaxElements, nullptr);
                seenId_ = s.lastProgramId;
                if (n <= 0 || !h_->runChain(els, n)) { fail("could not start the acceptance run"); return false; }
                stage_ = StageAccChain; run_ = Run::Busy;
                setMsg("home, 0 %%, 50 %%, 100 %%, home, then one random action");
                out_->printf("COMMISSION=step %d running: %s", Acceptance, msg_);
                return true;
            }
            default:
                out_->printf("COMMISSION=nothing to run in step %d (%s): the page's checklist, then #PCOMMISSIONNEXT", st.commissionStep, stepName(st.commissionStep));
                return false;
        }
    }

    // #PCOMMISSIONNEXT: advance if this step has passed.
    bool next(const Snapshot& s) {
        if (!active_) { out_->line("COMMISSION=not active (#PCOMMISSIONSTART)"); return false; }
        if (run_ == Run::Busy) { out_->line("COMMISSION=busy (wait for the step to finish)"); return false; }
        Settings& st = h_->settings();
        int cur = st.commissionStep;
        const char* why = nullptr;
        switch (cur) {
            case Profile: break;                                           // the acknowledgements are the page's
            case Wiring:
                if (s.topLimit && s.bottomLimit) why = "both limit switches read closed: check their wiring and polarity (#PLIFTERNC)";
                else if (s.faults & (F_LimitContradiction | F_LifterDiag | F_RotaryDiag | F_DiagRepeated | F_Expander)) why = "a driver or expander fault is showing: clear it first";
                else if (!h_->expanderOk()) why = "the I/O expander is not answering: the motors cannot be enabled";
                break;
            case SafeSpin: if (run_ != Run::Passed && !st.rotaryDisabled) why = "store the measured height first (#PSAFESPINHERE)"; break;
            case Creep: case Rotary: case Calibrate: case Acceptance:
                if (run_ != Run::Passed) why = "this step has not passed yet (#PCOMMISSIONRUN)"; break;
            default: why = "nothing to advance"; break;
        }
        if (why) { out_->printf("COMMISSION=step %d not done: %s", cur, why); return false; }
        if (cur == Acceptance) {
            st.commissionStep = Complete; st.commissioned = 2; active_ = false; clearRun();
            h_->settingsChanged();
            out_->line("COMMISSIONED=2 (commissioning complete; saved)");
            return true;
        }
        st.commissionStep = static_cast<uint8_t>(cur + 1);
        clearRun();
        h_->settingsChanged();
        out_->printf("COMMISSION=step %d/%d (%s)", st.commissionStep, kSteps, stepName(st.commissionStep));
        return true;
    }

    // #PSAFESPINHERE: the operator has lowered the turned head until it was just clear. Store that
    // height plus a margin. Only in step 6, with the head visibly turned and the mast below the top,
    // so the number can only come from the measurement.
    bool safeSpinHere(const Snapshot& s) {
        Settings& st = h_->settings();
        const char* why = nullptr;
        if (!active_ || st.commissionStep != SafeSpin) why = "only in commissioning step 6, after lowering the turned head";
        else if (run_ == Run::Busy || s.activeJob != JobKind::None) why = "wait until nothing moves";
        else if (s.estop) why = "E-STOP latched";
        else if (s.lifterConf != LifterConfidence::Known) why = "the lifter height is not known";
        else if (s.topLimit) why = "the mast is still at the top: lower it first";
        else if (s.homeConf == HomeConfidence::SwitchLive || s.homeConf == HomeConfidence::Unknown || s.ticksPerRev <= 0) why = "the head is not turned (or its angle is unknown): turn it at the top first";
        else { int d = s.rotaryDeg % 360; if (d < 0) d += 360; if (d > 180) d = 360 - d; if (d < kTurnedDeg) why = "the head is within 45 degrees of home: turn it at the top first"; }
        if (why) { out_->printf("SAFESPINHERE=refused (%s)", why); return false; }
        Ticks travel = s.lifterTravel > 0 ? s.lifterTravel : 1;
        Ticks margin = travel * 3 / 100; if (margin < 20) margin = 20;
        Ticks h = s.lifterTicks < 0 ? 0 : s.lifterTicks;
        Ticks v = h + margin;
        if (v >= travel) {
            st.safeSpinTicks = 0;
            setMsg("measured %ld + %ld margin reaches the top: only the top counts as clear (#PSAFESPIN0)", (long)h, (long)margin);
        } else {
            st.safeSpinTicks = static_cast<uint16_t>(v);
            setMsg("stored %ld ticks (%ld %%): measured %ld + %ld margin", (long)v, (long)(v * 100 / travel), (long)h, (long)margin);
        }
        detail_ = st.safeSpinTicks;
        run_ = Run::Passed;
        h_->settingsChanged();
        out_->printf("SAFESPIN=%u (saved; %s). Next: raise and bring the head home (:PA0), end the override (#POVERRIDE0)", st.safeSpinTicks, msg_);
        return true;
    }

    // ---------------------------------------------------------------- observation (every loop)

    void onEvent(const Event& e) {
        if (e.code == Ev::Estop && e.b) { if (active_ || run_ == Run::Busy) stop("E-STOP"); return; }
        if (run_ != Run::Busy) return;
        if (e.code == Ev::LimitTop && e.a == 1 && stage_ == StageCreep && upFromBottom_) detail_ = e.b;   // the raw count at the top switch = the travel the creep saw
        if (e.code == Ev::SeekClamped && (stage_ == StageAccChain || stage_ == StageAccRandom || stage_ == StageAccRandomWait))
            failBusy("a descent was clamped at %ld ticks (%s): the head was not home", (long)e.b, reasonName(e.c));
    }

    void update(const Snapshot& s, Ms now) {
        if (run_ != Run::Busy) return;
        switch (stage_) {
            case StageCreep: {
                if (!ended(s)) return;
                bool ok = s.lastProgramId == waitId_ && s.lastResult == JobResult::DoneLimit && s.topLimit;
                if (!ok) { failCreep(s, +1); return; }
                if (detail_ > 0) pass("reached the top switch; the creep saw %ld ticks from the bottom switch (travel in use: %ld)", (long)detail_, (long)s.lifterTravel);
                else pass("reached the top switch");
                return;
            }
            case StageRotChar: {
                if (!ended(s)) return;
                if (s.lastProgramId != waitId_) { failBusy("interrupted by another command"); return; }
                const CharacteriseResult& r = h_->characterisation();
                if (s.lastResult != JobResult::Done) {
                    if (s.lastReason == static_cast<uint8_t>(Reason::NoEdge)) failBusy("the home switch never closed in a turn and a quarter: check its wiring; a normally-closed switch needs #PROTARYNC1 and #PRESTART");
                    else failBusy("rotary test %s %s", resultName(static_cast<int>(s.lastResult)), reasonName(s.lastReason));
                    return;
                }
                if (!r.confident && !r.probable) { failBusy("the turns did not agree (spread %.1f %%): check the home switch and its mounting, then run again", r.spreadPct); return; }
                detail_ = static_cast<int32_t>(r.meanTpr * 10.f + 0.5f);
                MotionRequest h; h.kind = JobKind::RotaryHome;
                pushOrFail(h, StageRotHome, "%.1f ticks/rev (spread %.1f %%), contact arc %d ticks, %s; homing", r.meanTpr, r.spreadPct, r.arcMean, r.confident ? "confident" : "probable");
                return;
            }
            case StageRotHome: {
                if (!ended(s)) return;
                if (s.lastProgramId != waitId_ || s.lastResult != JobResult::Done) { failBusy("the home after the measurement did not finish (%s %s)", resultName(static_cast<int>(s.lastResult)), reasonName(s.lastReason)); return; }
                const CharacteriseResult& r = h_->characterisation();
                pass("%.1f ticks/rev (spread %.1f %%), contact arc %d ticks, %d closures over %d passes, %s; home found", r.meanTpr, r.spreadPct, r.arcMean, r.closures, r.detections, r.confident ? "confident" : "probable");
                return;
            }
            case StageCal: {
                if (!ended(s)) return;
                if (s.lastProgramId != waitId_ || s.lastResult != JobResult::Done) { failBusy("calibration %s %s", resultName(static_cast<int>(s.lastResult)), reasonName(s.lastReason)); return; }
                stage_ = StageCalStore; stageAt_ = now;
                return;
            }
            case StageCalStore: {
                const Calibration& c = h_->calibration();
                if (c.valid && (c.measuredAtMs != calAtStart_ || !calValidAtStart_)) {
                    detail_ = c.travel;
                    pass("travel %u ticks, lowest power up %u %% / down %u %% (stored)", c.travel, c.minUp / 10, c.minDown / 10);
                } else if (elapsed(now, stageAt_) > kCalStoreMs) failBusy("the calibration finished but was not stored");
                return;
            }
            case StageSpinRaise: {
                if (!ended(s)) return;
                if (s.lastProgramId != waitId_ || !s.topLimit) { failBusy("could not reach the top switch (%s %s)", resultName(static_cast<int>(s.lastResult)), reasonName(s.lastReason)); return; }
                MotionRequest r; r.kind = JobKind::RotaryAbsolute; r.a = kTurnDeg;   // at the top: the one height that is clear by construction
                pushOrFail(r, StageSpinTurn, "turning the head to %d degrees at the top", kTurnDeg);
                return;
            }
            case StageSpinTurn: {
                if (!ended(s)) return;
                if (s.lastProgramId != waitId_ || (s.lastResult != JobResult::DoneTarget && s.lastResult != JobResult::Done)) { failBusy("the turn did not finish (%s %s)", resultName(static_cast<int>(s.lastResult)), reasonName(s.lastReason)); return; }
                run_ = Run::Idle; stage_ = StageNone;
                setMsg("head turned %d degrees at the top. Turn the override on, then lower in small steps while watching; stop while the head is still clear and store that height", kTurnDeg);
                out_->printf("COMMISSION=%s", msg_);
                return;
            }
            case StageAccChain: case StageAccRandom: case StageAccRandomWait: {
                if (s.lastProgramId != seenId_) {
                    seenId_ = s.lastProgramId;
                    if (isFailure(s.lastResult)) { failBusy("%s %s %s", jobName(static_cast<int>(s.lastJob)), resultName(static_cast<int>(s.lastResult)), reasonName(s.lastReason)); return; }
                }
                bool quiet = !h_->chainActive() && s.activeJob == JobKind::None;
                if (stage_ == StageAccChain && quiet) {
                    actions0_ = h_->randomActions(); h_->randomStart(0); randomOurs_ = true; stage_ = StageAccRandom;
                    out_->line("COMMISSION=acceptance: one gentle random action");
                } else if (stage_ == StageAccRandom && h_->randomActions() > actions0_) {
                    stage_ = StageAccRandomWait;
                } else if (stage_ == StageAccRandomWait && quiet) {
                    h_->randomStop(); randomOurs_ = false;
                    pass("home, 0 %%, 50 %%, 100 %%, home and a random action all ran");
                }
                return;
            }
            default: return;
        }
    }

  private:
    enum Stage : uint8_t { StageNone = 0, StageCreep, StageRotChar, StageRotHome, StageCal, StageCalStore, StageSpinRaise, StageSpinTurn, StageAccChain, StageAccRandom, StageAccRandomWait };

    static bool isFailure(JobResult r) { return r == JobResult::Refused || r == JobResult::Aborted || r == JobResult::Stalled || r == JobResult::Failed || r == JobResult::FaultExit; }
    bool ended(const Snapshot& s) const { return waitId_ != 0 && static_cast<int32_t>(s.lastProgramId - waitId_) >= 0 && s.activeJob == JobKind::None; }
    static MotionRequest creepRequest(int dir) { MotionRequest r; r.kind = JobKind::LifterCreep; r.a = dir; return r; }   // the wizard creep: continuous, stops on its switch

    void clearRun() { run_ = Run::Idle; stage_ = StageNone; waitId_ = 0; msg_[0] = '\0'; detail_ = 0; }
    void setMsg(const char* fmt, ...) __attribute__((format(printf, 2, 3))) { va_list ap; va_start(ap, fmt); std::vsnprintf(msg_, sizeof msg_, fmt, ap); va_end(ap); }
    void pass(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        va_list ap; va_start(ap, fmt); std::vsnprintf(msg_, sizeof msg_, fmt, ap); va_end(ap);
        run_ = Run::Passed; stage_ = StageNone; waitId_ = 0;
        out_->printf("COMMISSION=step %d passed: %s (#PCOMMISSIONNEXT)", step(), msg_);
    }
    void fail(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        va_list ap; va_start(ap, fmt); std::vsnprintf(msg_, sizeof msg_, fmt, ap); va_end(ap);
        run_ = Run::Failed; stage_ = StageNone; waitId_ = 0;
        out_->printf("COMMISSION=step %d FAILED: %s", step(), msg_);
    }
    // a failure while busy also ends whatever the flow itself started
    void failBusy(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char m[sizeof msg_]; va_list ap; va_start(ap, fmt); std::vsnprintf(m, sizeof m, fmt, ap); va_end(ap);
        if (randomOurs_) { h_->randomStop(); randomOurs_ = false; }
        if (stage_ == StageAccChain || stage_ == StageAccRandom || stage_ == StageAccRandomWait) h_->cancelMotion();
        fail("%s", m);
    }
    void failCreep(const Snapshot& s, int dir) {
        if (s.lastProgramId != waitId_) { failBusy("interrupted by another command"); return; }
        Reason r = static_cast<Reason>(s.lastReason);
        if (r == Reason::WrongLimit) failBusy("the %s switch closed while creeping %s: the mechanism runs the other way. Flip 'runs backwards' (#PINVERT) and run again", dir > 0 ? "bottom" : "top", dir > 0 ? "up" : "down");
        else if (s.lastResult == JobResult::Stalled) failBusy("the lifter stalled creeping %s at %ld ticks: if it was sitting on its %s switch and did not move, it may run the other way (#PINVERT); otherwise check the wiring, the power and for binding", dir > 0 ? "up" : "down", (long)s.lifterTicks, dir > 0 ? "bottom" : "top");
        else failBusy("creep %s ended %s %s", dir > 0 ? "up" : "down", resultName(static_cast<int>(s.lastResult)), reasonName(s.lastReason));
    }
    bool pushOrFail(const MotionRequest& req, Stage st, const char* fmt, ...) __attribute__((format(printf, 4, 5))) {
        MotionRequest r = req; r.source = ReqSource::Commission;
        uint32_t id = h_->pushJob(r);
        if (id == 0) { fail("the command queue is full: try again"); return false; }
        va_list ap; va_start(ap, fmt); std::vsnprintf(msg_, sizeof msg_, fmt, ap); va_end(ap);
        waitId_ = id; stage_ = st; run_ = Run::Busy;
        out_->printf("COMMISSION=step %d running: %s", step(), msg_);
        return true;
    }

    Host* h_ = nullptr; TextSink* out_ = nullptr;
    bool active_ = false, randomOurs_ = false, calValidAtStart_ = false, upFromBottom_ = false;
    Run run_ = Run::Idle;
    Stage stage_ = StageNone;
    uint32_t waitId_ = 0, seenId_ = 0, actions0_ = 0;
    uint32_t calAtStart_ = 0;
    uint16_t prevSafeSpin_ = 0;
    Ms startedAt_ = 0, stageAt_ = 0;
    int32_t detail_ = 0;
    char msg_[160] = {};
};

} // namespace uppity
