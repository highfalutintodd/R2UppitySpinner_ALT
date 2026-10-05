#pragma once
#include <atomic>
#include "Units.h"
#include "Hal.h"
#include "Profiles.h"
#include "Knowledge.h"
#include "SafetyPolicy.h"
#include "FaultModel.h"
#include "EstopLatch.h"
#include "CommandQueue.h"
#include "StatusSnapshot.h"
#include "EventLog.h"
#include "MotionTypes.h"
#include "LifterAxis.h"
#include "RotaryAxis.h"
// The single motor authority (docs/REWRITE-PLAN.md §3.3, Appendix F). One instance, ticked at
// 1 kHz from the supervisor task (or from a test rig). Nothing else drives a motor.
//
// A request expands into a short program of steps (e.g. an absolute rotation on an
// unreferenced head becomes Home then Absolute). Steps run one at a time; a failed step ends
// the program with its outcome. Continuous jobs (spin, jog) are pre-empted by the next request.
namespace uppity {

// The slice of Settings the supervisor needs. Applied only between jobs.
struct MotionConfig {
    LifterMotor motor = LifterMotor::Pololu4757_6p3;
    bool     expertMode = false;
    Throttle minPowerUp = 0;        // 0 = profile default
    Throttle minPowerDown = 0;
    Throttle approachPower = 0;     // 0 = profile seek-bottom power
    Throttle jogPowerUp = 0;        // slow continuous power for jog and #PNUDGE (the calibrated minimum); 0 = profile default minimum
    Throttle jogPowerDown = 0;
    Ticks    travelTicks = 0;       // 0 = profile default
    bool     travelCalibrated = false;
    Ticks    bootBottomTicks = 0;   // the count at power-up on the bottom switch: minus how far below the
                                    // switch's closing point the mast rests (calibration); 0 = not measured
    bool     lifterInverted = false; // v3 fLifterInverted: mechanism runs backwards; negate drive AND count
    Ticks    safeSpinTicks = 0;     // 0 = only the top limit counts as clear (see safeSpin())
    RotaryCharacterisation rotary;
    bool     rotaryDisabled = false;
    bool     commissioned = false;
    bool     commissioningActive = false;   // the commissioning flow lifts the lock for its own steps
    Ms       idleDisableMs = 2000;
    uint8_t  driftPct = 5;                  // hold: re-seek when off by more than this; 0 = off
    uint16_t overshootUp[kSlots] = {};      // coast ticks per 5 % slot, from calibration
    uint16_t overshootDown[kSlots] = {};
    uint16_t velUp[kSlots] = {};            // ticks per second per slot, from calibration
    uint16_t velDown[kSlots] = {};
};

// Request encodings (MotionRequest a/b/c):
//   LifterSeek        a = target ticks, b = speed permille (0 = max), c bit0 = full travel to a limit
//   LifterCreep       a = direction (+1/-1), b = power permille (0 = default), c bit0 = jog (lease-based),
//                     c bit1 = step: a = signed ticks to travel (#PNUDGE), at the slow power unless b says otherwise
//   RotaryAbsolute    a = degrees 0..359, b = speed permille (0 = floor), c = max speed permille
//   RotaryRelative    a = signed degrees, b = speed, c = max speed
//   RotarySpin        a = signed raw throttle permille (motor polarity, ":PR" sense)
//   RotaryJog         a = signed raw throttle permille (rescue: 12 % floor, lease-based)
//   RotaryHome        c bit1 = skip if already home, bit2 = skip if the lifter is at/below safe height
//   RotaryCharacterise a = revolutions (1..10), c as RotaryHome; c bit4 (8) = home afterwards (the
//                     operator's #PROTARYTEST; commissioning queues its own home)
//   LifterCalibrate   - (raises and homes the rotary first when needed)
//   SafeLower         b = speed permille  (":PH": raise if unknown -> home if needed -> lights off -> lower)
//   LifterSeek        c bit1 = skip if the lifter position is already known (composites only)

class MotionSupervisor {
  public:
    // The drivers are switched on from loop() (I2cService): a loop() pass held up by a long print
    // (#PCONFIG on USB is ~60 ms) made a 50 ms wait fail with a sticky EXPANDER fault. Nothing moves
    // while it waits, so the wait can be generous; a dead expander still faults.
    static constexpr Ms kEnableTimeoutMs = 250;
    static constexpr int kLimitMakeTicks = 3, kLimitBreakTicks = 20;
    static constexpr int kHomeMakeTicks = 2, kHomeBreakTicks = 30;
    static constexpr Ms  kRestSwitchHoldMs = 400;    // at rest, a home-switch state must hold this long to count
    // A rest contact (zero the frame on a closed switch) needs the head still for this long: longer
    // than the switch's release debounce, so a head coasting off the switch after an interrupted
    // spin is never zeroed where it happens to be (code review 2026-10, finding 1).
    static constexpr Ms  kRestContactStillMs = 50;
    static constexpr Ms kTickEventMs = 1000;
    static constexpr Ms kEnCycleLowMs = 50;      // EN held low to clear the TB9051FTG latch
    static constexpr Ms kEnVerifyMs = 400;       // then enabled while DIAG is re-evaluated
    static constexpr int kMaxSteps = 4;
    // After an interrupted job the mechanism is still coasting; the next job waits until both
    // counts have been still for kRestStillTicks, or kRestMaxMs at most (a noisy encoder must
    // not block for ever). Bench 2026-09-27: a descent started 1 ms after a cancelled rise and
    // failed ENCODER_DIRECTION because the lead screw was still climbing.
    static constexpr int kRestStillTicks = 30;
    static constexpr Ms  kRestMaxMs = 400;
    static constexpr Ticks kMaxStepTicks = 100;   // longest #PNUDGE
    // E-STOP brakes; it does not let go. The short brake (both PWM low) only works while the
    // drivers are ENABLED: with EN low the TB9051FTG outputs float and the motor coasts. A lead
    // screw that back-drives under the mast's weight then keeps falling (bench 2026-10-03: ~560
    // ticks to the bottom switch after the latch, because the drivers went off on the first
    // E-STOP tick). So the drivers stay on, braking, until both counts have been still for
    // kEstopStillTicks, then switch off as after any stop; kEstopBrakeMaxMs is the backstop for
    // counts that never settle (the same 2 s an ordinary stop brakes for).
    static constexpr int kEstopStillTicks = 100;
    static constexpr Ms  kEstopBrakeMaxMs = 2000;
    // After a creep has stopped a turned head at the safe-spin height, a rise of this much (or a
    // home, or a new safe-spin setting) re-arms descents: from there it can brake early again.
    static constexpr Ticks kClampRearmTicks = 20;
    // A turned head sent down from close above the safe-spin height (Phase 5 Part A): from rest, a
    // seek's full-power breakaway gathers more speed than any estimate sees in time (the simulated
    // 19:1 started 9-30 ticks above came to rest up to 7 below). Within kClampRearmTicks it does not
    // move at all; up to kClampCreepTicks it goes the slow way, the creep Rescue's jog uses (bench
    // 2026-10-04: +4 above 1100); from farther up the seek brakes early with the floor's margin.
    static constexpr Ticks kClampCreepTicks = 60;

    MotionSupervisor(Hal& hal, EstopLatch& estop, CommandQueue& queue, SnapshotPublisher& pub, EventLog& events);

    void setConfig(const MotionConfig& c);   // any context; applied when idle
    void begin(Ms now);                      // boot resolution, no motion
    void tick(Ms now);
    void reportTickStats(uint32_t cycleUs, bool late);
    void requestClearFault() { clearFaultReq_ = true; }
    // Rescue "set rotary home here": the operator says the head is lined up with the opening.
    // Applied at the next tick if nothing is running and the E-STOP is not latched, else refused
    // (logged). Any context.
    void requestSetRotaryHome() { setHomeReq_.store(true, std::memory_order_release); }
    bool alignmentSuspect() const { return alignmentSuspect_; }
    // The head may be turned (off home above the safe height, or turned below it, since its last
    // home), whatever the height: what the console checks before switching the rotary off.
    bool headMayBeTurned() const { return turnedSinceHome_; }
    // Persist holds the intake around an NVS commit (a flash erase stalls both cores), the OTA
    // paths for a whole upload: no new program starts while any holder is set; queued requests
    // simply wait. One bit per holder, so a settings save that ends mid-upload cannot release the
    // upload's hold. Any context.
    static constexpr uint8_t kHoldPersist = 1, kHoldOta = 2, kHoldRestart = 4;
    void setHoldIntake(uint8_t owner, bool hold) {
        if (hold) holdIntake_.fetch_or(owner, std::memory_order_acq_rel); else holdIntake_.fetch_and(static_cast<uint8_t>(~owner), std::memory_order_acq_rel);
    }
    bool holdIntake() const { return holdIntake_.load(std::memory_order_acquire) != 0; }
    uint32_t lastProgramId() const { return lastProgramId_; }

    // Read-only views for tests, the console and persistence.
    const Snapshot& current() const { return snap_; }
    JobKind activeJob() const { return job_; }
    const LifterKnowledge& lifter() const { return lifter_; }
    const RotaryKnowledge& rotary() const { return rotary_; }
    const FaultModel& faults() const { return faults_; }
    const MotionConfig& config() const { return cfg_; }
    const LifterMotorProfile& profile() const { return *prof_; }
    const HomeResult& lastHome() const { return lastHome_; }
    const CharacteriseResult& lastCharacterisation() const { return lastChar_; }
    const CalibrateResult& lastCalibration() const { return lastCal_; }
    bool takeCharacterisationUpdated() { bool u = charUpdated_; charUpdated_ = false; return u; }
    bool takeCalibrationUpdated() { bool u = calUpdated_; calUpdated_ = false; return u; }
    RotaryHome::Phase homePhase() const { return rhome_.phase(); }
    LifterCalibrate::Phase calibratePhase() const { return calib_.phase(); }
    Ticks holdTarget() const { return holdTarget_; }

  private:
    struct Step { JobKind kind = JobKind::None; int32_t a = 0, b = 0, c = 0; };
    struct Debounce { uint8_t make = 0, brk = 0; bool state = false; };

    void debounceSwitch(Debounce& d, bool raw, int makeTicks, int breakTicks);
    void applyPendingConfig();
    bool invertApplied_ = false;
    void updateKnowledge(const TickInputs& in, bool rotaryBusy);
    int  expandRequest(const MotionRequest& r, const TickInputs& in, Step* steps);
    int  startStep(const Step& s, const TickInputs& in);     // 1 started, 0 skipped, -1 refused
    void runProgram(const TickInputs& in);                   // start the next runnable step
    bool lifterAboveSafeNow(const TickInputs& in) const;
    void watchHomeSwitch(const TickInputs& in, bool rotaryBusy);
    bool attendedSource(bool forUnlock) const;
    void stepJob(const TickInputs& in, BridgeCommand& lifterOut, BridgeCommand& rotaryOut);
    void endStep(const StepOutcome& o, const TickInputs& in);
    void endProgram(const StepOutcome& o, const TickInputs& in);
    void abortJob(JobResult how, Reason why, const TickInputs& in);
    void refuseAll(Reason why, const TickInputs& in);
    void refuse(const Step& s, Reason why, uint32_t id, const TickInputs& in);
    PolicyContext policy(const TickInputs& in) const;
    void markAlignmentSuspect(Ms now);
    int  startFloorCreep(Ticks floor, const TickInputs& in);
    void updateAlignment(const TickInputs& in);
    Ticks travel() const;
    Ticks safeSpin() const;
    Ticks tpr() const;
    Throttle rotaryFloor() const { return pctToThrottle(cfg_.rotary.minPowerPct); }
    Throttle floorSpeed(Throttle speed, int dir) const;
    bool rotaryReferenced() const { return rotary_.confidence == HomeConfidence::Latched || rotary_.confidence == HomeConfidence::SwitchLive || rotary_.confidence == HomeConfidence::Stale; }
    bool rotaryHomeKnown() const { return rotary_.confidence == HomeConfidence::Latched || rotary_.confidence == HomeConfidence::SwitchLive; }
    bool isRotaryJob(JobKind k) const { return k == JobKind::RotaryAbsolute || k == JobKind::RotaryRelative || k == JobKind::RotarySpin || k == JobKind::RotaryJog || k == JobKind::RotaryHome || k == JobKind::RotaryCharacterise; }
    bool isPreemptible(JobKind k) const { return k == JobKind::RotarySpin || k == JobKind::RotaryJog || k == JobKind::LifterCreep; }
    // a plain seek or rotation is interrupted by the next request (the page's Down while rising must
    // not wait for the rise); a home, a characterisation or a calibration always finishes
    bool isRetargetable(JobKind k) const { return k == JobKind::LifterSeek || k == JobKind::RotaryAbsolute || k == JobKind::RotaryRelative; }
    void publish(const TickInputs& in, BridgeCommand lifterOut, BridgeCommand rotaryOut);
    void emit(Ev code, int32_t a, int32_t b, int32_t c, Ms t) { events_.push(code, a, b, c, t); }

    Hal& hal_;
    EstopLatch& estop_;
    CommandQueue& queue_;
    SnapshotPublisher& pub_;
    EventLog& events_;

    MotionConfig cfg_, pendingCfg_;
    bool cfgPending_ = false;
    const LifterMotorProfile* prof_;

    LifterKnowledge lifter_;
    RotaryKnowledge rotary_;
    FaultModel faults_;
    Debounce top_, bottom_, home_;
    bool topWas_ = false, botWas_ = false, homeWas_ = false;
    bool homePending_ = false; Ms homePendingSince_ = 0;
    Ticks rotStillAt_ = 0; Ms rotStillSince_ = 0;  // the rotary count and since when it has not moved (rest contact)
    Ticks rotLast_ = 0;                            // the rotary count at the end of the last tick, in the current frame
    Ticks swClosedRun_ = 0;                        // how far the count has moved while the home switch read closed without a break
    Ms swOpenSince_ = 0; bool swOpen_ = false;     // the switch reading open at rest clears HOME_SWITCH_STUCK

    // program + job slot
    Step steps_[kMaxSteps];
    int stepCount_ = 0, stepIdx_ = 0;
    uint32_t programId_ = 0;
    ReqSource programSource_ = ReqSource::None;   // who asked for the running program (the override and the commissioning unlock are for attended sources)
    JobKind programKind_ = JobKind::None;
    JobKind job_ = JobKind::None;
    LifterSeek seek_;
    LifterCreep creep_;
    RotaryMove rmove_;
    RotarySpin rspin_;
    RotaryHome rhome_;
    RotaryCharacterise rchar_;
    LifterCalibrate calib_;
    HomeResult lastHome_;
    CharacteriseResult lastChar_;
    CalibrateResult lastCal_;
    bool charUpdated_ = false, calUpdated_ = false;
    bool suppressLimitRef_ = false;   // during calibration the raw count must survive the limit edges
    // hold / drift correction
    Ticks holdTarget_ = -1;
    Ms holdCheckAt_ = 0;
    int holdFails_ = 0;
    bool arming_ = false;
    Ms armingSince_ = 0;
    Ms idleSince_ = 0;
    bool wasStopped_ = false;
    bool estopBraking_ = false;           // E-STOP latched with the drivers on: braking until both axes are still
    Ms estopAt_ = 0;
    Ticks estopL_ = 0, estopR_ = 0;       // counts at the latch, for the "how far did it go" log line
    bool clearFaultReq_ = false;
    std::atomic<bool> setHomeReq_{false};
    bool turnedSinceHome_ = false;        // the head was off home above the safe height (or turned below it) since its last home
    bool alignmentSuspect_ = false;       // turnedSinceHome_ while below the safe height (or clamp-held): what the page and the log show
    bool jogByOverride_ = false;          // the running jog was allowed only because the override was on (D4)
    bool clampHold_ = false;              // a creep came to rest at the safe-spin height with the head turned (SafetyPolicy clampHeld)
    Ticks clampAt_ = 0, clampFloor_ = 0;
    uint8_t restReport_ = 0;              // 6 nudge / 7 clamped creep or seek / 8 seek: log where it came to rest once the counts are still
    Ticks restFrom_ = 0, restFloor_ = 0;  // where it started; the floor (7) or the target (8)
    int32_t restAsked_ = 0;
    Ms restSince_ = 0;
    bool overrideWas_ = false;            // for the OVERRIDE on/off log lines (set, expired, cancelled, cleared by E-STOP)
    std::atomic<uint8_t> holdIntake_{0};
    uint32_t lastProgramId_ = 0;
    bool restRequired_ = false; Ticks stillL_ = 0, stillR_ = 0; uint16_t stillTicks_ = 0;
    uint8_t enCycle_ = 0;
    Ms enCycleAt_ = 0;
    bool enableRequested_ = false;
    Ms enableRequestedAt_ = 0;
    bool lastEnabled_ = false;
    Ms enabledSince_ = 0;

    // output stage
    BridgeCommand lastOut_[2];
    int lastDriveSign_[2] = {0, 0};

    // status
    Snapshot snap_;
    MotionState state_ = MotionState::Locked;
    JobResult lastResult_ = JobResult::None;
    Reason lastReason_ = Reason::None;
    JobKind lastJob_ = JobKind::None;
    uint32_t cycleUsMax_ = 0, lateTicks_ = 0, tickCount_ = 0;
    Ms lastTickEvent_ = 0;
};

} // namespace uppity
