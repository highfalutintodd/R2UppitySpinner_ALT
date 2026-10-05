// Test rig: simulated board + the real supervisor, stepped 1 ms at a time.
#pragma once
#include "uppity_test.h"
#include "../../src/sim/SimHal.h"
#include "../../src/core/MotionSupervisor.h"

namespace uppity { namespace rig {

struct Rig {
    sim::SimHal hal;
    NoLock lock;
    CommandQueue queue{lock};
    SnapshotPublisher pub;
    EventLog events;
    EstopLatch estop;
    MotionSupervisor sup{hal, estop, queue, pub, events};
    std::vector<Event> log;

    static void panic(void* ctx) { static_cast<sim::SimHal*>(ctx)->panicStop(); }

    explicit Rig(MotionConfig cfg = MotionConfig(), bool gregFixture = true) {
        estop.setPanic(panic, &hal);
        if (gregFixture) { sim::LifterParams p; p.breakawayDuty = 0.45; p.travelTicks = 450; p.kv = 0.5; hal.lifter.setParams(p); }
        sup.setConfig(cfg);
    }
    // Ground-truth safety watch (code review 2026-10): with watchSafe set to the safe-spin height in
    // the simulated mast's own ticks, every simulated millisecond counts a rotary drive while the
    // mast is below it (top switch not made), and a descent below it with the head more than
    // 10.6 degrees off the switch centre. It reads the mechanism, not the firmware's idea of it, so a
    // test that only checks where things ended cannot miss a violation on the way. Off (< 0) by
    // default; a test that uses the override covers only the moves the override does not.
    double watchSafe = -1;
    int rotaryBelowSafe = 0, turnedBelowSafe = 0;
    double headOffDegReal() const {
        const auto& rp = hal.rotary.params();
        double a = std::fmod(hal.rotary.position() - rp.camCentreTicks, rp.ticksPerRev); if (a < 0) a += rp.ticksPerRev;
        if (a > rp.ticksPerRev / 2) a -= rp.ticksPerRev;
        return std::fabs(a) * 360.0 / rp.ticksPerRev;
    }
    void boot() { sup.begin(hal.nowMs()); drain(); }
    // Advance one ms: tick at time t, then let the mechanism respond for 1 ms.
    void stepOnce() {
        double before = hal.lifter.position();
        sup.tick(hal.nowMs()); hal.advance(1); drain();
        if (watchSafe >= 0) {
            double now = hal.lifter.position();
            BridgeCommand rc = hal.command(Axis::Rotary);
            if (rc.mode == Bridge::Drive && rc.duty != 0 && now < watchSafe && !hal.lifter.topLimit()) ++rotaryBelowSafe;
            if (now < before - 0.01 && now < watchSafe && headOffDegReal() > 10.6) ++turnedBelowSafe;
        }
    }
    void runMs(int n) { for (int i = 0; i < n; ++i) stepOnce(); }
    void runUntilIdle(int maxMs) {
        for (int i = 0; i < maxMs; ++i) { stepOnce(); if (sup.activeJob() == JobKind::None && queue.size() == 0) return; }
    }
    void drain() { Event e; while (events.pop(e)) log.push_back(e); }
    void clearRecordsAndLog() { hal.clearRecords(); log.clear(); }
    uint32_t seek(Ticks target, int speedPermille = 0, bool fullTravel = false) {
        MotionRequest r; r.kind = JobKind::LifterSeek; r.a = target; r.b = speedPermille; r.c = fullTravel ? 1 : 0; r.source = ReqSource::Test; return queue.push(r);
    }
    uint32_t creep(int dir, int power, bool jog) {
        MotionRequest r; r.kind = JobKind::LifterCreep; r.a = dir; r.b = power; r.c = jog ? 1 : 0; r.source = ReqSource::Test; return queue.push(r);
    }
    uint32_t req(JobKind k, int32_t a = 0, int32_t b = 0, int32_t c = 0) {
        MotionRequest r; r.kind = k; r.a = a; r.b = b; r.c = c; r.source = ReqSource::Test; return queue.push(r);
    }
    // Where the cam centre sits in the reported (firmware) frame, folded into (-tpr/2, tpr/2].
    // Zero means the reference is exactly on the switch centre.
    Ticks homeErrorTicks() const {
        Snapshot s = snap();
        Ticks offset = s.rotaryTicks - hal.rotary.encoder();
        double tpr = hal.rotary.params().ticksPerRev;
        Ticks centre = static_cast<Ticks>(std::floor(hal.rotary.params().camCentreTicks)) + offset;
        Ticks m = static_cast<Ticks>(std::fmod(static_cast<double>(centre), tpr)); if (m < 0) m += static_cast<Ticks>(tpr);
        if (m > tpr / 2) m -= static_cast<Ticks>(tpr);
        return m;
    }
    void setRotaryAngleFromCentre(double ticksFromCentre) { hal.rotary.setPosition(hal.rotary.params().camCentreTicks + ticksFromCentre); }
    int count(Ev code) const { int n = 0; for (auto& e : log) if (e.code == code) ++n; return n; }
    const Event* last(Ev code) const { for (int i = static_cast<int>(log.size()) - 1; i >= 0; --i) if (log[i].code == code) return &log[i]; return nullptr; }
    Snapshot snap() const { Snapshot s; pub.read(s); return s; }
    // Records after time t: any drive command?
    bool anyDriveAfter(Ms t) const { for (int i = 0; i < hal.recordCount(); ++i) { const auto& r = hal.rec(i); if (r.t > t && r.c.mode == Bridge::Drive && r.c.duty != 0) return true; } return false; }
    int brakesBetween(Ms a, Ms b) const { int n = 0; for (int i = 0; i < hal.recordCount(); ++i) { const auto& r = hal.rec(i); if (r.t >= a && r.t <= b && r.a == Axis::Lifter && r.c.mode == Bridge::Brake) ++n; } return n; }
};

inline MotionConfig gregConfig(bool rotaryDisabled = true) {
    MotionConfig c;
    c.motor = LifterMotor::Pololu4757_6p3;
    c.travelTicks = 450; c.travelCalibrated = true;
    c.safeSpinTicks = 225;
    c.commissioned = true;
    c.rotaryDisabled = rotaryDisabled;
    c.rotary.ticksPerRev = 1184.6f; c.rotary.confident = true;
    c.rotary.driveSign = +1;                  // the simulator's rotary counts up under positive drive
    return c;
}
// Todd's lifter as the bench measured it (2026-10-03/04): ~1.4 ticks/ms flat out, travel 1150; with
// the drivers off a falling mast barely slows (gravity against running friction), at rest it holds,
// and the short brake stops it within tens of ticks (here about speed x 15 ms).
inline sim::LifterParams backdrivingLifter() {
    sim::LifterParams p;
    p.breakawayDuty = 0.45; p.travelTicks = 1150; p.kv = 1.4;
    p.tauCoastMs = 1e9; p.backdriveGravity = 0.004; p.backdriveFriction = 0.0049; // floating: slows ~0.001 ticks/ms per ms
    p.tauBrakeMs = 15;
    return p;
}
inline MotionConfig backdrivingConfig() { MotionConfig c = gregConfig(); c.travelTicks = 1150; c.safeSpinTicks = 575; return c; }
// A calibration table whose stopping distances say "brake `ms` ahead" (what #PSC would store).
inline void setBrakeTable(MotionConfig& c, int ms) {
    for (int i = 3; i <= 17; ++i) { c.velUp[i] = c.velDown[i] = 1000; c.overshootUp[i] = c.overshootDown[i] = static_cast<uint16_t>(ms); }
}

// The small-rotary community build: ~250 ticks per revolution, tighter cam geometry.
inline sim::RotaryParams smallRotary() {
    sim::RotaryParams p; p.ticksPerRev = 250.0; p.camCentreTicks = 60; p.halfWidthRest = 1.0; p.halfWidthMoving = 2.5; p.kv = 0.35; p.chatterP = 0.25;
    return p;
}

}} // namespace uppity::rig
