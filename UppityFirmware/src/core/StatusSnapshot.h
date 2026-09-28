#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include "Units.h"
#include "Knowledge.h"
// The one status document every reader (web, serial, LED, mesh) consumes. Published by the
// supervisor tick under a seqlock; readers never block the tick and never see a torn copy.
namespace uppity {

enum class MotionState : uint8_t {
    Locked = 0,    // commissioning incomplete: both motors refused
    Idle,
    Moving,
    Stopped,       // E-STOP latched
    Fault,
};

enum class JobKind : uint8_t {
    None = 0, LifterSeek, LifterCreep, LifterCalibrate, LifterHold,
    RotaryAbsolute, RotaryRelative, RotarySpin, RotaryJog, RotaryHome, RotaryCharacterise,
    SafeLower, RaiseThenRotate, FirstUse, BootResolve,
};

enum class JobResult : uint8_t {
    None = 0, Running, Done, DoneLimit, DoneTarget, Clamped, Refused, Aborted, Stalled, Failed, FaultExit,
};

struct Snapshot {
    uint32_t seq = 0;            // publisher's sequence (even = stable)
    Ms       tMs = 0;
    MotionState state = MotionState::Locked;

    Ticks lifterTicks = 0;
    int16_t lifterPct = 0;       // 0..100 of travel (travel = calibrated or profile default)
    LifterConfidence lifterConf = LifterConfidence::Unknown;
    Ticks lifterTravel = 0;

    Ticks rotaryTicks = 0;
    int16_t rotaryDeg = 0;       // 0..359, valid when ticksPerRev known
    HomeConfidence homeConf = HomeConfidence::Unknown;
    HomeProvenance homeProv = HomeProvenance::None;
    float ticksPerRev = 0.f;

    bool topLimit = false, bottomLimit = false, homeSwitch = false;
    bool lifterDiag = false, rotaryDiag = false;
    bool enabled = false;        // drivers enabled (expander acked)
    bool commissioned = false;
    bool rotaryDisabled = false;

    uint32_t faults = 0;         // FaultModel bitmask
    bool estop = false;
    uint8_t estopSource = 0;

    Ms overrideRemainingMs = 0;  // rescue override lease
    bool alignmentSuspect = false;   // the override let the head turn or descend unaligned; descents need a home first
    bool commissioning = false;  // the commissioning flow has lifted the lock for its steps
    JobKind activeJob = JobKind::None;
    uint32_t activeJobId = 0;
    JobKind activeProgram = JobKind::None;   // the request the active job belongs to (":PH" runs SEEK and HOME steps inside SAFE_LOWER)
    uint8_t jobPhase = 0;        // the active job's phase (seek / home / calibration), for progress displays
    int16_t jobDetail = 0;       // calibration: LifterCalibrate::progress(); characterisation: switch passes; creep: signed step or direction
    uint32_t lastProgramId = 0;  // id of the last program that ended (done, failed or refused); the sequencer waits on it
    JobKind lastJob = JobKind::None;
    JobResult lastResult = JobResult::None;
    uint8_t lastReason = 0;      // Reason code (see EventLog.h)

    Throttle lifterOut = 0;      // last commanded throttle (diagnostic)
    Throttle rotaryOut = 0;

    uint32_t cycleUsMax = 0;     // tick health
    uint32_t lateTicks = 0;
    uint32_t tickCount = 0;
};

// Single writer, many readers. Writer: begin (odd), copy, end (even). Reader: retry until the
// sequence is even and unchanged across the copy.
class SnapshotPublisher {
  public:
    void publish(const Snapshot& s) {
        uint32_t v = seq_.load(std::memory_order_relaxed);
        seq_.store(v + 1, std::memory_order_release);            // odd: write in progress
        std::atomic_thread_fence(std::memory_order_release);
        data_ = s;
        data_.seq = v + 2;
        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(v + 2, std::memory_order_release);            // even: stable
    }
    // Returns false only if a consistent copy could not be obtained in `tries` attempts.
    bool read(Snapshot& out, int tries = 8) const {
        for (int i = 0; i < tries; ++i) {
            uint32_t a = seq_.load(std::memory_order_acquire);
            if (a & 1u) continue;
            std::atomic_thread_fence(std::memory_order_acquire);
            Snapshot copy = data_;
            std::atomic_thread_fence(std::memory_order_acquire);
            uint32_t b = seq_.load(std::memory_order_acquire);
            if (a == b) { out = copy; return true; }
        }
        return false;
    }
    uint32_t seq() const { return seq_.load(std::memory_order_acquire); }

  private:
    std::atomic<uint32_t> seq_{0};
    Snapshot data_{};
};

} // namespace uppity
