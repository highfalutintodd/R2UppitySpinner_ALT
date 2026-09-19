#pragma once
#include <cstdint>
#include "Units.h"
#include "Hal.h"
#include "StatusSnapshot.h"
#include "EventLog.h"
// Shared types for the motion state machines.
namespace uppity {

// What every machine sees each tick: debounced, gated, and already policy-annotated inputs.
struct TickInputs {
    Ms    nowMs = 0;
    Ticks lifterTicks = 0;
    Ticks rotaryTicks = 0;
    bool  topLimit = false, bottomLimit = false;   // debounced "made"
    bool  homeSwitch = false;                      // debounced closed
    bool  homeSwitchRaw = false;
    bool  lifterDiag = false, rotaryDiag = false;  // debounced and gated on enable
    bool  estop = false, cancel = false;
    bool  enabled = false;                         // drivers enabled (acked)
    bool  jogLeaseLive = false;
};

enum class StepResult : uint8_t { Running, Done, Failed };

struct StepOutcome {
    StepResult result = StepResult::Running;
    JobResult  jobResult = JobResult::Running;
    Reason     reason = Reason::None;
    int32_t    detail = 0;
    static StepOutcome running() { return StepOutcome{}; }
    static StepOutcome done(JobResult r, Reason why = Reason::None, int32_t d = 0) { StepOutcome o; o.result = StepResult::Done; o.jobResult = r; o.reason = why; o.detail = d; return o; }
    static StepOutcome failed(JobResult r, Reason why, int32_t d = 0) { StepOutcome o; o.result = StepResult::Failed; o.jobResult = r; o.reason = why; o.detail = d; return o; }
};

inline BridgeCommand driveCmd(Throttle t) { BridgeCommand c; c.mode = Bridge::Drive; c.duty = t; return c; }
inline BridgeCommand brakeCmd() { BridgeCommand c; c.mode = Bridge::Brake; c.duty = 0; return c; }
inline BridgeCommand coastCmd() { BridgeCommand c; c.mode = Bridge::Coast; c.duty = 0; return c; }

// The lead-screw soft-stop pattern: on for onTicks, brake for offTicks, repeat. offTicks 0 = continuous.
struct PulsePattern {
    uint8_t onTicks = 3, offTicks = 1;
    uint16_t phase = 0;
    void set(uint8_t on, uint8_t off) { onTicks = on ? on : 1; offTicks = off; phase = 0; }
    bool tickOn() {
        uint16_t period = static_cast<uint16_t>(onTicks + offTicks);
        bool on = phase < onTicks;
        phase = static_cast<uint16_t>((phase + 1) % period);
        return on;
    }
};

// Universal exits shared by every machine. Returns true if the machine must stop now.
inline bool universalExit(const TickInputs& in, bool axisDiag, StepOutcome& out) {
    if (in.estop)  { out = StepOutcome::failed(JobResult::Aborted, Reason::Estop); return true; }
    if (in.cancel) { out = StepOutcome::failed(JobResult::Aborted, Reason::Cancel); return true; }
    if (axisDiag)  { out = StepOutcome::failed(JobResult::FaultExit, Reason::DiagFault); return true; }
    return false;
}

} // namespace uppity
