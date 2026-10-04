#pragma once
#include <atomic>
#include <cstdint>
#include "Units.h"
// Fixed-size single-producer/single-consumer ring of coded events. The supervisor pushes one
// record per transition or edge (never throttled, never formatted); loop() drains and formats.
// Overflow drops the newest and counts it, so silence in the log is never mistaken for calm.
namespace uppity {

enum class Ev : uint8_t {
    None = 0,
    Boot,               // a = lifter conf, b = home conf, c = limits bitmask
    StateChange,        // a = old MotionState, b = new
    JobStart,           // a = JobKind, b = param, c = job id
    JobEnd,             // a = JobKind, b = JobResult, c = reason
    SeekClamped,        // a = requested ticks, b = clamped ticks, c = reason
    LimitTop,           // a = 1 made / 0 released / 2 made but ignored (other limit still made), b = ticks
    LimitBottom,
    HomeSwitch,         // a = 1 closed / 0 open, b = rotary ticks
    HomeEdge,           // a = enter ticks, b = exit ticks, c = closures
    HomeFrameCorrected, // a = delta ticks
    ConfidenceLifter,   // a = old, b = new
    ConfidenceRotary,
    Estop,              // a = source, b = 1 tripped / 0 released
    Fault,              // a = fault id, b = 1 set / 0 cleared, c = detail
    Enable,             // a = 1 on / 0 off
    Persist,            // a = 1 done / 0 deferred
    Phase,              // a = JobKind, b = phase id, c = detail (seek/home/calibrate sub-steps)
    Measure,            // a = what, b = value, c = value2 (characterisation, calibration)
    Refused,            // a = JobKind, b = reason
    Override,           // a = 1 on / 0 off, b = ms
    LightKit,           // a = requested light kit mode (loop() applies it over I2C)
    Calibration,        // a = phase, b = value, c = value2
    Hold,               // a = 1 re-seek / 0 gave up, b = target ticks, c = current
    Tick,               // a = cycleUsMax, b = lateTicks (periodic health)
    HomeSettle,         // a = 1 contact at rest / 0 not, b = wiggle moves, c = nudges
    HomeManual,         // a = rotary ticks before, b = old HomeConfidence ("set rotary home here", Rescue)
    Alignment,          // a = 1 suspect (the head may be turned below the safe height) / 0 clear, b = lifter ticks, c (clear) = 0 homed / 1 above the safe height, not homed yet
    EstopSettled,       // a = lifter ticks moved after the latch, b = rotary ticks, c = ms of braking before the drivers switched off (bit 16: not seen still)
};

// Reason codes shared by JobResult/Refused/SeekClamped/JobEnd.
enum class Reason : uint8_t {
    None = 0,
    Estop, Cancel, LimitReached, TargetReached, Stall, SlowProgress, DiagFault, Timeout,
    RotaryNotHome, LifterUnknown, BelowSafeHeight, Locked, LeaseExpired, ExpanderFault,
    NoEdge, SwitchStuck, NotRepeatable, PolicyChange, Overshoot, AlreadyThere, HomeFailed,
    EncoderDirection, LimitContradiction, Busy, RotaryDisabled, EnableTimeout, Breakaway,
    AlreadyHome, Skipped, HoldGaveUp,
    WrongLimit,         // a creep met the limit switch at the end it was moving away from: the mechanism runs the other way
};

struct Event {
    Ev      code = Ev::None;
    int32_t a = 0, b = 0, c = 0;
    Ms      tMs = 0;
};

template <int N>
class EventRing {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");
  public:
    bool push(const Event& e) {
        uint32_t h = head_.load(std::memory_order_relaxed);
        uint32_t t = tail_.load(std::memory_order_acquire);
        if (h - t >= static_cast<uint32_t>(N)) { dropped_.fetch_add(1, std::memory_order_relaxed); return false; }
        buf_[h & (N - 1)] = e;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }
    bool push(Ev code, int32_t a, int32_t b, int32_t c, Ms t) { Event e; e.code = code; e.a = a; e.b = b; e.c = c; e.tMs = t; return push(e); }
    bool pop(Event& out) {
        uint32_t t = tail_.load(std::memory_order_relaxed);
        uint32_t h = head_.load(std::memory_order_acquire);
        if (t == h) return false;
        out = buf_[t & (N - 1)];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }
    uint32_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    uint32_t pending() const { return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire); }

  private:
    Event buf_[N];
    std::atomic<uint32_t> head_{0}, tail_{0}, dropped_{0};
};

using EventLog = EventRing<64>;

} // namespace uppity
