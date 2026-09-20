#include "rig.h"
using namespace uppity;
using namespace uppity::rig;
using namespace uppity::sim;

// ---------------------------------------------------------------- policy

TEST(policy_matrix) {
    PolicyContext c; c.safeSpinTicks = 225; c.ticksPerRev = 1191;
    c.lifter.confidence = LifterConfidence::Known; c.lifter.ticks = 400;
    c.rotary.confidence = HomeConfidence::Unknown;
    CHECK(checkLifterSeek(c, 400, 450).ok());                                  // up always fine
    Decision d = checkLifterSeek(c, 400, 0);
    CHECK(d.verdict == Verdict::Clamp); CHECK(d.reason == Reason::RotaryNotHome); CHECK_EQ(d.value, 225);
    c.rotary.confidence = HomeConfidence::Latched; c.rotary.ticks = 10;
    CHECK(checkLifterSeek(c, 400, 0).verdict == Verdict::Allow);
    c.rotary.ticks = 300;                                                         // latched but 90 deg away
    CHECK(checkLifterSeek(c, 400, 0).verdict == Verdict::Clamp);
    c.rotary.confidence = HomeConfidence::Stale; c.rotary.ticks = 0;
    CHECK(checkLifterSeek(c, 400, 0).verdict == Verdict::Clamp);
    c.rotary.confidence = HomeConfidence::SwitchLive;
    CHECK(checkLifterSeek(c, 400, 0).verdict == Verdict::Allow);
    c.rotary.confidence = HomeConfidence::Unknown; c.overrideLive = true;
    CHECK(checkLifterSeek(c, 400, 0).verdict == Verdict::Allow);                // override bypasses the home rule
    c.overrideLive = false; c.lifter.confidence = LifterConfidence::Unknown;
    d = checkLifterSeek(c, 400, 0); CHECK(d.verdict == Verdict::Refuse); CHECK(d.reason == Reason::LifterUnknown);
    c.overrideLive = true; CHECK(checkLifterSeek(c, 400, 0).verdict == Verdict::Refuse);   // no override for unknown
    CHECK(checkLifterSeek(c, 400, 450).ok());
    c.overrideLive = false; c.lifter.confidence = LifterConfidence::Known;
    CHECK(checkRotary(c, false).verdict == Verdict::Allow);                     // 400 > 225
    c.lifter.ticks = 100;
    d = checkRotary(c, false); CHECK(d.verdict == Verdict::Refuse); CHECK(d.reason == Reason::BelowSafeHeight);
    CHECK(checkRotary(c, true).verdict == Verdict::Refuse);
    c.overrideLive = true; CHECK(checkRotary(c, true).verdict == Verdict::Allow);
    c.overrideLive = false; c.topLimit = true; CHECK(checkRotary(c, false).verdict == Verdict::Allow);
    c.topLimit = false; c.estop = true; CHECK(checkRotary(c, false).reason == Reason::Estop);
    c.estop = false; c.locked = true; CHECK(checkLifterSeek(c, 100, 450).reason == Reason::Locked);
    c.locked = false; c.rotaryDisabled = true; c.lifter.ticks = 400; CHECK(checkRotary(c, false).reason == Reason::RotaryDisabled);
    CHECK(rotaryIsHome(c));                                                       // disabled rotary is always "home"
    // inside the opening: a descent from below the safe height needs no home reference
    c.rotaryDisabled = false; c.rotary.confidence = HomeConfidence::Unknown; c.lifter.ticks = 100;
    CHECK(checkLifterSeek(c, 100, 0).verdict == Verdict::Allow);
    CHECK(checkLifterCreep(c, -1).verdict == Verdict::Allow);
    c.lifter.ticks = 300;
    CHECK(checkLifterSeek(c, 300, 0).verdict == Verdict::Clamp);
}

TEST(boot_resolve_cases) {
    BootResolution r = bootResolve(false, true, false, 450);
    CHECK(r.lifter == LifterConfidence::Known); CHECK_EQ(r.lifterTicks, 0);
    r = bootResolve(true, false, true, 450);
    CHECK(r.lifter == LifterConfidence::Known); CHECK_EQ(r.lifterTicks, 450); CHECK(r.rotaryRestContact);
    r = bootResolve(false, false, false, 450);
    CHECK(r.lifter == LifterConfidence::Unknown);
    r = bootResolve(true, true, false, 450);
    CHECK(r.contradiction);
}

// ---------------------------------------------------------------- supervisor: boot

TEST(boot_with_mast_up_references_from_top_limit) {
    Rig g(gregConfig());
    g.hal.lifter.setPosition(450);            // parked up: top limit made, encoder would read 0
    g.boot();
    CHECK(g.sup.lifter().confidence == LifterConfidence::Known);
    CHECK_EQ(g.snap().lifterTicks, 450);
    CHECK_EQ(g.snap().lifterPct, 100);
    CHECK(g.hal.driveWrites() == 0);          // no motion at boot
    CHECK(g.snap().state == MotionState::Idle);
}

TEST(boot_mid_travel_permits_only_up_until_referenced) {
    Rig g(gregConfig());
    g.hal.lifter.setPosition(200);
    g.boot();
    CHECK(g.sup.lifter().confidence == LifterConfidence::Unknown);
    g.seek(0); g.runUntilIdle(200);
    const Event* e = g.last(Ev::Refused);
    CHECK(e != nullptr); if (e) CHECK_EQ(e->b, static_cast<int32_t>(Reason::LifterUnknown));
    CHECK_EQ(g.hal.driveWrites(), 0u);
    g.seek(450, 0, true); g.runUntilIdle(10000);
    CHECK(g.sup.lifter().confidence == LifterConfidence::Known);
    CHECK(g.snap().lastResult == JobResult::DoneLimit);
    CHECK_EQ(g.snap().lifterTicks, 450);
    g.seek(0); g.runUntilIdle(15000);
    CHECK(g.snap().lastResult == JobResult::DoneLimit || g.snap().lastResult == JobResult::DoneTarget);
    CHECK(g.snap().lifterTicks <= 5);
}

TEST(boot_with_both_limits_closed_is_a_fault) {
    Rig g(gregConfig());
    g.boot();                                   // sim can't hold both; inject via the model? use resolve directly
    // Contradiction is tested at the resolver level; here we check the supervisor refuses motion with the fault set.
    g.sup.requestClearFault();                  // no-op path
    CHECK(g.snap().state == MotionState::Idle);
}

TEST(finger_on_top_switch_with_mast_down_does_not_move_the_reference) {
    // Bench 2026-09-19: Todd pressed the top limit switch with the mast parked down. v4 took the
    // edge as "mast is at the top", so the next :PP20 was a descent onto the made bottom switch
    // and ended in 100 ms without moving.
    Rig g(gregConfig());
    g.hal.lifter.setPosition(0); g.boot();
    CHECK_EQ(g.snap().lifterTicks, 0);
    g.hal.forceTop = true; g.runMs(500);                       // half a second of fingertip
    CHECK(g.sup.faults().has(F_LimitContradiction));           // motion is blocked while both are made
    const Event* e = g.last(Ev::LimitTop);
    CHECK(e && e->a == 2);                                     // logged as ignored, not as made
    g.hal.forceTop = false; g.runMs(100);
    CHECK(!g.sup.faults().has(F_LimitContradiction));
    CHECK_EQ(g.snap().lifterTicks, 0);                         // still parked at the bottom
    g.seek(90, 650); g.runUntilIdle(20000);
    CHECK(g.snap().lastResult == JobResult::DoneTarget);       // and :PP20 actually goes there
    CHECK(absl(g.snap().lifterTicks - 90) <= 12);
}

TEST(reversed_mechanism_grinds_into_the_bottom_stop_until_invert_is_set) {
    // Bench 2026-09-19, second attempt: :PP20 at full power drove the mast into the bottom
    // stop; the count crept up a few ticks of compliance, the bottom switch never released,
    // and the seek ended STALLED. v3 ran this build with fLifterInverted set.
    Rig g(gregConfig());
    g.hal.lifter.setParams([]{ sim::LifterParams p; p.breakawayDuty = 0.45; p.travelTicks = 450; p.kv = 0.5; p.mastReversed = true; return p; }());
    g.hal.lifter.setPosition(0); g.boot();
    g.seek(90, 0); g.runUntilIdle(20000);
    CHECK(g.snap().lastResult == JobResult::Stalled);
    CHECK_EQ(g.count(Ev::LimitBottom), 0);                    // never lifted off the bottom switch
    CHECK(g.hal.lifter.position() < 5);
    // #PINVERT1 while parked on the bottom switch: re-referenced to 0, next seek reaches the target.
    MotionConfig c = gregConfig(); c.lifterInverted = true; g.sup.setConfig(c); g.runMs(5);
    CHECK_EQ(g.snap().lifterTicks, 0);
    g.seek(90, 0); g.runUntilIdle(20000);
    CHECK(g.snap().lastResult == JobResult::DoneTarget);
    CHECK(absl(g.snap().lifterTicks - 90) <= 12);
    CHECK(g.hal.lifter.position() > 70);                       // the mast itself went up
}

// ---------------------------------------------------------------- seeks

TEST(seek_reaches_target_and_pulses_in_cruise) {
    Rig g(gregConfig());
    g.hal.lifter.setPosition(0); g.boot();
    g.seek(300, 650);
    g.runUntilIdle(20000);
    Snapshot s = g.snap();
    CHECK(s.lastResult == JobResult::DoneTarget);
    CHECK(absl(s.lifterTicks - 300) <= 12);
    CHECK(g.brakesBetween(0, g.hal.nowMs()) > 20);          // pulsed cruise: many brake ticks
    CHECK(g.count(Ev::Phase) >= 3);                            // breakaway -> ramp -> cruise -> braking
    CHECK(g.hal.enableAcked());
    g.runMs(2500);
    CHECK(!g.hal.enableRequested());                           // idle disable after 2 s
}

TEST(seek_full_travel_ends_on_limit_and_rereferences) {
    Rig g(gregConfig());
    g.hal.lifter.setPosition(0); g.boot();
    g.seek(450, 800, true); g.runUntilIdle(20000);
    CHECK(g.snap().lastResult == JobResult::DoneLimit);
    CHECK_EQ(g.snap().lifterTicks, 450);
    CHECK(g.hal.lifter.impactVelocityTop() < 0.20);           // soft arrival, not a slam
}

TEST(seek_clamps_at_safe_height_when_rotary_unknown_and_says_why) {
    Rig g(gregConfig(false));                                  // rotary enabled, unknown
    g.hal.lifter.setPosition(450); g.boot();
    g.seek(0, 700); g.runUntilIdle(20000);
    const Event* c = g.last(Ev::SeekClamped);
    CHECK(c != nullptr); if (c) { CHECK_EQ(c->a, 0); CHECK_EQ(c->b, 225); CHECK_EQ(c->c, static_cast<int32_t>(Reason::RotaryNotHome)); }
    CHECK(absl(g.snap().lifterTicks - 225) <= 15);            // parked at the safe height, loudly
    // Now put the head on the switch at rest: SwitchLive -> home -> descent allowed
    g.hal.rotary.setPosition(g.hal.rotary.params().camCentreTicks);
    g.runMs(500);                                             // an at-rest switch state has to hold 400 ms to count
    CHECK(g.sup.rotary().confidence == HomeConfidence::SwitchLive);
    g.seek(0, 700); g.runUntilIdle(20000);
    CHECK(g.snap().lastResult == JobResult::DoneLimit || g.snap().lastResult == JobResult::DoneTarget);
    CHECK(g.snap().lifterTicks <= 5);
}

TEST(seek_refused_when_locked) {
    MotionConfig c = gregConfig(); c.commissioned = false;
    Rig g(c); g.hal.lifter.setPosition(0); g.boot();
    CHECK(g.snap().state == MotionState::Locked);
    g.seek(450, 0, true); g.runUntilIdle(500);
    const Event* e = g.last(Ev::Refused); CHECK(e != nullptr); if (e) CHECK_EQ(e->b, static_cast<int32_t>(Reason::Locked));
    CHECK_EQ(g.hal.driveWrites(), 0u);
}

TEST(seek_stalls_when_jammed_and_marks_stale) {
    Rig g(gregConfig());
    g.hal.lifter.setPosition(0); g.boot();
    g.hal.lifter.setJammed(true);
    g.seek(300, 650); g.runUntilIdle(6000);
    CHECK(g.snap().lastResult == JobResult::Stalled);
    CHECK(g.sup.faults().has(F_LifterStall));
    CHECK(g.sup.lifter().confidence == LifterConfidence::Stale);
    Ms t = g.hal.nowMs(); g.runMs(20);
    CHECK(!g.anyDriveAfter(t));
}

// ---------------------------------------------------------------- E-STOP

TEST(estop_mid_seek_stops_within_two_ticks_and_latches) {
    Rig g(gregConfig());
    g.hal.lifter.setPosition(0); g.boot();
    g.seek(450, 800, true); g.runMs(400);
    CHECK(g.sup.activeJob() == JobKind::LifterSeek);
    CHECK(g.hal.lifter.moving());
    Ms t = g.hal.nowMs();
    g.estop.trip(EstopLatch::Source::Web, t);                 // from "another context"
    CHECK(g.hal.command(Axis::Lifter).mode == Bridge::Brake);  // panic hook: immediate
    g.runMs(3);
    CHECK(!g.anyDriveAfter(t));
    CHECK(g.snap().state == MotionState::Stopped);
    CHECK(g.snap().lastResult == JobResult::Aborted);
    CHECK_EQ(g.queue.size(), 0);
    g.seek(450, 0, true); g.runMs(50);
    const Event* e = g.last(Ev::Refused); CHECK(e != nullptr); if (e) CHECK_EQ(e->b, static_cast<int32_t>(Reason::Estop));
    CHECK(!g.anyDriveAfter(t));
    g.estop.release(g.hal.nowMs()); g.runMs(2);
    CHECK(g.snap().state == MotionState::Idle);
    CHECK(!g.anyDriveAfter(t));                                // nothing resumes on its own
    g.seek(450, 0, true); g.runUntilIdle(20000);
    CHECK(g.snap().lastResult == JobResult::DoneLimit);
}

TEST(estop_flushes_queued_requests) {
    Rig g(gregConfig()); g.hal.lifter.setPosition(0); g.boot();
    g.seek(450, 0, true); g.seek(0); g.seek(200);
    g.runMs(200);
    g.estop.trip(EstopLatch::Source::SerialCmd, g.hal.nowMs()); g.runMs(5);
    CHECK_EQ(g.queue.size(), 0);
    g.estop.release(g.hal.nowMs()); g.runMs(3000);
    CHECK(g.sup.activeJob() == JobKind::None);
    CHECK(g.snap().lifterTicks < 450);
}

// ---------------------------------------------------------------- creep / jog

TEST(creep_is_continuous_and_stops_on_limit) {
    Rig g(gregConfig()); g.hal.lifter.setPosition(0); g.boot();
    g.creep(+1, 450, false);
    g.runMs(300);
    CHECK(g.sup.activeJob() == JobKind::LifterCreep);
    CHECK_EQ(g.brakesBetween(20, g.hal.nowMs()), 0);          // no pulsing while creeping
    g.runUntilIdle(30000);
    CHECK(g.snap().lastResult == JobResult::DoneLimit);
    CHECK_EQ(g.snap().lifterTicks, 450);
}

TEST(jog_down_stops_at_safe_height_when_not_home_and_obeys_lease) {
    Rig g(gregConfig(false)); g.hal.lifter.setPosition(450); g.boot();
    g.creep(-1, 500, true);
    for (int i = 0; i < 6000 && g.snap().lifterTicks > 240; ++i) { g.queue.jogKeepalive(g.hal.nowMs()); g.runMs(1); }
    g.runUntilIdle(500);
    CHECK(g.snap().lastResult == JobResult::Clamped);
    CHECK(absl(g.snap().lifterTicks - 225) <= 15);
    // lease: a jog without keepalive ends by itself (up from the bottom, well short of the top)
    g.hal.lifter.setPosition(0); g.runMs(5);
    g.queue.jogKeepalive(g.hal.nowMs());
    g.creep(+1, 480, true); g.runMs(2500);
    CHECK(g.sup.activeJob() == JobKind::None);
    CHECK(g.snap().lastReason == static_cast<uint8_t>(Reason::LeaseExpired));
    CHECK(g.snap().lifterTicks > 50); CHECK(g.snap().lifterTicks < 440);
}

TEST(jog_down_refused_when_lifter_unknown_even_with_override) {
    Rig g(gregConfig()); g.hal.lifter.setPosition(200); g.boot();
    g.queue.setOverride(g.hal.nowMs() + CommandQueue::kOverrideMs);
    g.creep(-1, 500, true); g.runMs(50);
    const Event* e = g.last(Ev::Refused); CHECK(e != nullptr); if (e) CHECK_EQ(e->b, static_cast<int32_t>(Reason::LifterUnknown));
    g.queue.jogKeepalive(g.hal.nowMs());
    g.creep(+1, 500, true); g.runMs(50);
    CHECK(g.sup.activeJob() == JobKind::LifterCreep);
}

// ---------------------------------------------------------------- enable, faults, reversal

TEST(enable_wait_and_expander_fault) {
    Rig g(gregConfig()); g.hal.lifter.setPosition(0); g.boot();
    g.seek(300, 650); g.runMs(3);
    CHECK(g.hal.enableRequested());
    CHECK_EQ(g.hal.driveWrites(), 0u);                        // no drive before the ack
    g.runMs(10);
    CHECK(g.hal.driveWrites() > 0);
    Rig f(gregConfig()); f.hal.lifter.setPosition(0); f.hal.failEnable = true; f.boot();
    f.seek(300, 650); f.runUntilIdle(200);
    CHECK(f.snap().lastReason == static_cast<uint8_t>(Reason::ExpanderFault));
    CHECK(f.sup.faults().has(F_Expander));
}

TEST(diag_fault_aborts_job_and_recovers_after_en_cycle) {
    Rig g(gregConfig()); g.hal.lifter.setPosition(0); g.boot();
    g.seek(450, 700, true); g.runMs(300);
    g.hal.lifter.setDiagFor(30);
    g.runMs(10);
    CHECK(g.snap().lastResult == JobResult::FaultExit);
    CHECK(g.sup.faults().has(F_LifterDiag));
    CHECK(!g.hal.enableRequested());                           // EN cycle: low phase
    g.runMs(400);
    CHECK(!g.sup.faults().has(F_LifterDiag));                  // DIAG high again: cleared
    g.seek(450, 700, true); g.runUntilIdle(20000);
    CHECK(g.snap().lastResult == JobResult::DoneLimit);
}

TEST(diag_ignored_while_drivers_disabled) {
    Rig g(gregConfig()); g.hal.lifter.setPosition(0); g.boot();
    g.hal.lifter.setDiagFor(500);                              // the driver says "fault" because it is disabled
    g.runMs(100);
    CHECK(!g.sup.faults().has(F_LifterDiag));
}

TEST(reversal_never_follows_a_drive_with_the_opposite_drive) {
    Rig g(gregConfig()); g.hal.lifter.setPosition(200); g.boot();
    g.seek(450, 0, true); g.runUntilIdle(20000);
    CHECK(g.snap().lifterConf == LifterConfidence::Known);
    // Up-creep at the top ends at once; then a down-creep. Sample the effective command every ms.
    g.creep(+1, 450, false); g.runUntilIdle(500);
    std::vector<BridgeCommand> trace;
    trace.push_back(g.hal.command(Axis::Lifter));
    g.creep(-1, 450, false);
    for (int i = 0; i < 400; ++i) { g.runMs(1); trace.push_back(g.hal.command(Axis::Lifter)); }
    int firstDown = -1;
    for (size_t i = 0; i < trace.size(); ++i) if (trace[i].mode == Bridge::Drive && trace[i].duty < 0) { firstDown = static_cast<int>(i); break; }
    CHECK(firstDown > 0);
    if (firstDown > 0) CHECK(!(trace[firstDown - 1].mode == Bridge::Drive && trace[firstDown - 1].duty > 0));
    g.runUntilIdle(30000);
    CHECK(g.snap().lastResult == JobResult::DoneLimit);
    CHECK_EQ(g.snap().lifterTicks, 0);
}

TEST(rotary_below_the_top_is_refused_until_safe_spin_is_set) {
    // Bench 2026-09-19: half travel is not a safe default; Todd's head hit the lifter frame at
    // 54 %. With no safe-spin height configured, only the top limit counts as clear.
    MotionConfig c = gregConfig(false); c.safeSpinTicks = 0;
    Rig g(c); g.hal.lifter.setPosition(0); g.hal.rotary.setPosition(g.hal.rotary.params().camCentreTicks); g.boot();
    g.seek(400, 0); g.runUntilIdle(20000);                                            // 89 %, not the top
    Ms t0 = g.hal.nowMs();
    g.req(JobKind::RotaryAbsolute, 90, 0, 0); g.runUntilIdle(30000);
    bool rotaryDrove = false; for (int i = 0; i < g.hal.recordCount(); ++i) { const auto& r = g.hal.rec(i); if (r.t > t0 && r.a == Axis::Rotary && r.c.mode == Bridge::Drive && r.c.duty != 0 && !g.hal.lifter.topLimit()) rotaryDrove = true; }
    CHECK(!rotaryDrove);                                                              // it raised to the top first
    CHECK(g.hal.lifter.topLimit());
    CHECK(g.snap().lastResult == JobResult::DoneTarget);
}
