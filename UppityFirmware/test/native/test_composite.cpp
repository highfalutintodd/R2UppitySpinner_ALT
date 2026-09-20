#include "rig.h"
using namespace uppity;
using namespace uppity::rig;
using namespace uppity::sim;

static int starts(const Rig& g, JobKind k) { int n = 0; for (auto& e : g.log) if (e.code == Ev::JobStart && e.a == (int)k) ++n; return n; }

TEST(ph_from_raised_with_unhomed_rotary_homes_then_lowers) {
    Rig g(gregConfig(false)); g.hal.lifter.setPosition(450); g.hal.rotary.setPosition(500); g.boot();
    CHECK(g.sup.rotary().confidence == HomeConfidence::Unknown);
    g.req(JobKind::SafeLower, 0, 700, 0); g.runUntilIdle(120000);
    CHECK_EQ(starts(g, JobKind::RotaryHome), 1);
    CHECK_EQ(starts(g, JobKind::LifterSeek), 1);                     // no raise needed, one descent
    CHECK(g.sup.rotary().confidence == HomeConfidence::Latched || g.sup.rotary().confidence == HomeConfidence::SwitchLive);   // settled on contact = SWITCH_LIVE
    CHECK(g.snap().lastResult == JobResult::DoneLimit || g.snap().lastResult == JobResult::DoneTarget);
    CHECK(g.snap().lifterTicks <= 5);
    CHECK_EQ(g.count(Ev::LightKit), 1);
    const Event* lk = g.last(Ev::LightKit); if (lk) CHECK_EQ(lk->a, 1);
}

TEST(ph_with_broken_switch_stays_raised_and_says_why) {
    Rig g(gregConfig(false)); g.hal.lifter.setPosition(450); g.hal.rotary.setPosition(500); g.hal.rotary.setSwitchDisconnected(true); g.boot();
    g.req(JobKind::SafeLower, 0, 700, 0); g.runUntilIdle(120000);
    CHECK(g.snap().lastResult == JobResult::Failed);
    CHECK(g.snap().lastReason == (uint8_t)Reason::HomeFailed);
    CHECK(g.snap().lifterTicks >= 440);                              // never descended
    CHECK_EQ(starts(g, JobKind::LifterSeek), 0);
    { const Event* e = g.last(Ev::JobEnd); CHECK(e && e->a == (int)JobKind::SafeLower && e->c == (int)Reason::HomeFailed); }   // the log says so too
}

TEST(ph_from_below_safe_height_skips_homing) {
    Rig g(gregConfig(false)); g.hal.lifter.setPosition(0); g.hal.rotary.setPosition(500); g.boot();
    g.seek(100, 700); g.runUntilIdle(20000);
    CHECK(absl(g.snap().lifterTicks - 100) <= 12);
    g.req(JobKind::SafeLower, 0, 700, 0); g.runUntilIdle(120000);
    CHECK_EQ(starts(g, JobKind::RotaryHome), 0);
    CHECK(g.snap().lifterTicks <= 5);
    CHECK(g.snap().lastResult == JobResult::DoneLimit || g.snap().lastResult == JobResult::DoneTarget);
}

TEST(ph_from_unknown_position_raises_first) {
    Rig g(gregConfig(false)); g.hal.lifter.setPosition(300); g.hal.rotary.setPosition(500); g.boot();
    CHECK(g.sup.lifter().confidence == LifterConfidence::Unknown);
    g.req(JobKind::SafeLower, 0, 700, 0); g.runUntilIdle(200000);
    CHECK_EQ(starts(g, JobKind::LifterSeek), 2);                     // raise, then lower
    CHECK_EQ(starts(g, JobKind::RotaryHome), 1);
    CHECK(g.snap().lifterTicks <= 5);
}

TEST(rotary_request_below_safe_height_raises_first) {
    Rig g(gregConfig(false)); g.hal.lifter.setPosition(0); g.hal.rotary.setPosition(g.hal.rotary.params().camCentreTicks); g.boot();
    g.req(JobKind::RotaryRelative, 90, 500, 0); g.runUntilIdle(120000);
    CHECK_EQ(starts(g, JobKind::LifterSeek), 1);
    CHECK_EQ(g.snap().lifterTicks, 450);
    CHECK(g.snap().lastResult == JobResult::DoneTarget || g.snap().lastResult == JobResult::Done);
    CHECK(circularDistance(g.snap().rotaryDeg, 90) <= 6);
}

TEST(descent_from_inside_opening_needs_no_home) {
    Rig g(gregConfig(false)); g.hal.lifter.setPosition(0); g.hal.rotary.setPosition(500); g.boot();
    g.seek(100, 700); g.runUntilIdle(20000);
    g.seek(0, 700); g.runUntilIdle(20000);
    CHECK_EQ(g.count(Ev::SeekClamped), 0);
    CHECK(g.snap().lifterTicks <= 5);
}

TEST(calibration_measures_travel_min_power_and_slots) {
    Rig g(gregConfig(true)); g.hal.lifter.setPosition(0);
    MotionConfig c = gregConfig(true); c.travelTicks = 0; c.travelCalibrated = false; g.sup.setConfig(c);
    g.boot();
    g.req(JobKind::LifterCalibrate); g.runUntilIdle(900000);
    CHECK(g.snap().lastResult == JobResult::Done);
    if (g.snap().lastResult != JobResult::Done) std::printf("   calibration ended result=%d reason=%d phase=%d ticks=%ld t=%u\n", (int)g.snap().lastResult, (int)g.snap().lastReason, (int)g.sup.calibratePhase(), (long)g.snap().lifterTicks, (unsigned)g.hal.nowMs());
    const CalibrateResult& r = g.sup.lastCalibration();
    CHECK(r.travelOk);
    CHECK(absl(r.travel - 450) <= 9);
    CHECK(r.travelSpreadPct < 2.f);
    CHECK(r.minUp >= 400 && r.minUp <= 600);                         // fixture breaks away at 45 %
    CHECK(r.minDown >= 400 && r.minDown <= 600);
    CHECK(r.slotsMeasured >= 12);
    CHECK(r.slotValidUp[20] && r.slotValidUp[12]);
    CHECK(r.velUp[20] > r.velUp[12]);
    CHECK(g.sup.takeCalibrationUpdated());
    CHECK_EQ(g.sup.config().travelTicks, r.travel);
    CHECK(g.sup.config().travelCalibrated);
    CHECK_EQ(g.snap().lifterTicks, 0);                               // ends referenced at the bottom
    CHECK(g.sup.lifter().confidence == LifterConfidence::Known);
}

TEST(calibration_rejects_non_repeatable_travel) {
    Rig g(gregConfig(true)); g.hal.lifter.setPosition(0);
    LifterParams p; p.breakawayDuty = 0.45; p.travelTicks = 450; p.kv = 0.5; p.phantomPerMsDriving = 0.35; g.hal.lifter.setParams(p);
    g.boot();
    g.req(JobKind::LifterCalibrate); g.runUntilIdle(900000);
    CHECK(g.snap().lastResult == JobResult::Failed);
    CHECK(g.snap().lastReason == (uint8_t)Reason::NotRepeatable);
    CHECK(!g.sup.takeCalibrationUpdated());
    CHECK_EQ(g.sup.config().travelTicks, 450);                       // stored value kept
}

TEST(calibration_locked_unless_commissioning) {
    MotionConfig c = gregConfig(true); c.commissioned = false;
    Rig g(c); g.hal.lifter.setPosition(0); g.boot();
    g.req(JobKind::LifterCalibrate); g.runMs(50);
    const Event* e = g.last(Ev::Refused); CHECK(e != nullptr); if (e) CHECK_EQ(e->b, (int)Reason::Locked);
    c.commissioningActive = true; g.sup.setConfig(c); g.runMs(5);
    g.req(JobKind::LifterCalibrate); g.runMs(200);
    CHECK(g.sup.activeJob() == JobKind::LifterCalibrate);
    g.estop.trip(EstopLatch::Source::Test, g.hal.nowMs()); g.runMs(5);
    CHECK(g.snap().lastResult == JobResult::Aborted);
}

TEST(hold_recorrects_drift_and_gives_up_after_three_failures) {
    Rig g(gregConfig(true)); g.hal.lifter.setPosition(0); g.boot();
    g.seek(300, 650); g.runUntilIdle(30000);
    CHECK_EQ(g.sup.holdTarget(), 300);
    g.hal.lifter.setPosition(255); g.runMs(6000);                     // 10 % drift, above the 5 % threshold
    g.runUntilIdle(30000);
    CHECK(g.count(Ev::Hold) >= 1);
    CHECK(absl(g.snap().lifterTicks - 300) <= 12);
    CHECK_EQ(g.sup.holdTarget(), 300);                               // still holding
    g.hal.lifter.setJammed(true); g.hal.lifter.setPosition(255);
    g.runMs(40000);                                                   // three failed re-seeks
    CHECK(g.sup.holdTarget() < 0);
    const Event* h = g.last(Ev::Hold); CHECK(h != nullptr); if (h) CHECK_EQ(h->a, 0);
}

TEST(safe_lower_lands_on_the_bottom_switch_even_when_travel_is_wrong) {
    // Bench 2026-09-19: profile travel 450 on a ~1150-tick lifter. :PH descended to count 0 and
    // stopped halfway with the bottom switch never made. The descent must end on the switch.
    MotionConfig c = gregConfig(); c.travelTicks = 450;
    Rig g(c);
    g.hal.lifter.setParams([]{ sim::LifterParams p; p.breakawayDuty = 0.45; p.travelTicks = 1150; p.kv = 0.5; return p; }());
    g.hal.lifter.setPosition(1150); g.boot();                 // parked at the real top: count = 450
    g.req(JobKind::SafeLower); g.runUntilIdle(30000);
    CHECK(g.snap().lastResult == JobResult::DoneLimit);
    CHECK(g.hal.lifter.bottomLimit());
    CHECK_EQ(g.snap().lifterTicks, 0);
}

TEST(ph_with_unknown_ticks_per_rev_characterises_then_homes_then_lowers) {
    // Bench 2026-09-19: first boot with the head away from home and no ticks/rev. :PH ran the
    // two-revolution characterisation, left the head ~50 ticks past the switch, and the descent
    // clamped at safe height (ROTARY_NOT_HOME). Characterisation must be followed by a home.
    MotionConfig c = gregConfig(false); c.rotary.ticksPerRev = 0.f; c.rotary.confident = false; c.travelTicks = 450;
    Rig g(c);
    g.hal.rotary.setParams([]{ sim::RotaryParams p; p.tauBrakeMs = 40.0; return p; }());   // a head that coasts ~15 ticks at floor power, as Todd's does, so the last pass overruns the switch
    g.hal.lifter.setPosition(450); g.hal.rotary.setPosition(500); g.boot();
    g.req(JobKind::SafeLower); g.runUntilIdle(60000);
    CHECK(g.count(Ev::Measure) >= 2);                          // characterisation happened
    CHECK(g.count(Ev::HomeEdge) >= 1 || absl(g.homeErrorTicks()) <= 12);   // a real home unless the head already sat within tolerance
    CHECK(absl(g.homeErrorTicks()) <= 12);
    CHECK(g.snap().lastResult == JobResult::DoneLimit);
    CHECK(g.hal.lifter.bottomLimit());
    CHECK_EQ(g.count(Ev::SeekClamped), 0);
}

TEST(ph_from_the_bottom_reports_already_there) {
    Rig g(gregConfig(false)); g.hal.lifter.setPosition(0); g.hal.rotary.setPosition(g.hal.rotary.params().camCentreTicks); g.boot();
    g.req(JobKind::SafeLower); g.runUntilIdle(5000);
    CHECK(!g.anyDriveAfter(0));
    const Event* e = g.last(Ev::JobEnd);
    CHECK(e && e->a == (int)JobKind::SafeLower && e->b == (int)JobResult::Done && e->c == (int)Reason::AlreadyThere);
}
