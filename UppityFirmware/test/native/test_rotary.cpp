#include "rig.h"
using namespace uppity;
using namespace uppity::rig;
using namespace uppity::sim;

// Lifter parked at the top (Known, above safe height), rotary enabled, no reference yet.
static Rig* upRig(bool small = false) {
    Rig* g = new Rig(gregConfig(false));
    if (small) { g->hal.rotary.setParams(smallRotary()); MotionConfig c = gregConfig(false); c.rotary.ticksPerRev = 250.f; g->sup.setConfig(c); }
    g->hal.lifter.setPosition(450);
    return g;
}

// ---------------------------------------------------------------- homing

TEST(home_finds_edge_from_every_start_angle_both_fixtures) {
    for (int fixture = 0; fixture < 2; ++fixture) {
        double tpr = fixture ? 250.0 : 1184.6;
        for (int k = 0; k < 13; ++k) {
            Rig* g = upRig(fixture == 1);
            double frac = k / 12.0;                                   // 0 .. 360 degrees incl. on-switch
            g->setRotaryAngleFromCentre(frac * tpr);
            g->boot();
            g->req(JobKind::RotaryHome);
            g->runUntilIdle(120000);
            bool done = g->snap().lastResult == JobResult::Done;
            CHECK(done);
            CHECK(g->sup.rotary().confidence == HomeConfidence::Latched || g->sup.rotary().confidence == HomeConfidence::SwitchLive);
            Ticks err = g->homeErrorTicks();
            CHECK(absl(err) <= (fixture ? 3 : 8));
            if (!done || absl(err) > (fixture ? 3 : 8)) std::printf("   fixture=%d k=%d result=%d reason=%d err=%ld swept=%ld\n", fixture, k, (int)g->snap().lastResult, (int)g->snap().lastReason, (long)err, (long)g->sup.lastHome().swept);
            delete g;
        }
    }
}

TEST(home_search_bound_is_distance_not_time) {
    Rig* g = upRig(true);
    g->hal.rotary.setSwitchDisconnected(true);
    g->boot();
    Ms t0 = g->hal.nowMs();
    g->req(JobKind::RotaryHome);
    g->runUntilIdle(120000);
    CHECK(g->snap().lastResult == JobResult::Failed);
    CHECK(g->snap().lastReason == static_cast<uint8_t>(Reason::NoEdge));
    const Event* e = g->last(Ev::JobEnd);
    CHECK(e != nullptr);
    CHECK(g->sup.lastHome().swept == 0);                               // no home was recorded
    CHECK(elapsed(g->hal.nowMs(), t0) < 50000u);                      // both sweeps done well inside the backstop
    // it swept at least 1.25 rev in each direction: the head travelled ~2.5 rev in total
    CHECK(g->hal.rotary.travelled() > 2.4 * 250.0);
    CHECK(g->hal.rotary.travelled() < 3.2 * 250.0);
    delete g;
}

TEST(home_zero_is_centred_for_both_approach_directions) {
    Rig* g = upRig();
    g->setRotaryAngleFromCentre(-60); g->boot();
    g->req(JobKind::RotaryHome); g->runUntilIdle(120000);
    CHECK(g->snap().lastResult == JobResult::Done);
    Ticks e1 = g->homeErrorTicks();
    // second home from the other side; pass 1 exists now (referenced) and always sweeps the same way
    g->setRotaryAngleFromCentre(+60); g->runMs(5);
    g->req(JobKind::RotaryHome); g->runUntilIdle(120000);
    CHECK(g->snap().lastResult == JobResult::Done);
    Ticks e2 = g->homeErrorTicks();
    CHECK(absl(e1) <= 8); CHECK(absl(e2) <= 8);
    CHECK(absl(e1 - e2) <= 4);
    delete g;
}

TEST(failed_home_never_rezeroes) {
    Rig* g = upRig();
    g->setRotaryAngleFromCentre(200); g->boot();
    g->hal.setEncoder(Axis::Rotary, 500);                             // some frame
    g->runMs(2);
    Ticks before = g->snap().rotaryTicks - g->hal.rotary.encoder();
    g->hal.rotary.setSwitchDisconnected(true);
    g->req(JobKind::RotaryHome); g->runUntilIdle(120000);
    CHECK(g->snap().lastResult == JobResult::Failed);
    Ticks after = g->snap().rotaryTicks - g->hal.rotary.encoder();
    CHECK_EQ(before, after);
    CHECK(g->sup.rotary().confidence != HomeConfidence::Latched);
    delete g;
}

TEST(rest_contact_corrects_a_wrong_frame_loudly) {
    Rig* g = upRig();
    g->setRotaryAngleFromCentre(100); g->boot();
    g->req(JobKind::RotaryHome); g->runUntilIdle(120000);
    CHECK(g->snap().lastResult == JobResult::Done);
    // homing now ends resting on the switch, so lift the head off it first (the frame stays LATCHED)
    g->setRotaryAngleFromCentre(40); g->runMs(500);
    CHECK(g->sup.rotary().confidence == HomeConfidence::Latched);
    int base = g->count(Ev::HomeFrameCorrected);
    // corrupt the frame by 130 ticks (~40 degrees), then park the head on the switch
    g->hal.setEncoder(Axis::Rotary, g->snap().rotaryTicks + 130); g->runMs(2);
    CHECK(g->count(Ev::HomeFrameCorrected) == base);
    g->setRotaryAngleFromCentre(0.5); g->runMs(600);
    CHECK(g->count(Ev::HomeFrameCorrected) == base + 1);
    CHECK(absl(g->snap().rotaryTicks) <= 2);
    CHECK(g->sup.rotary().confidence == HomeConfidence::SwitchLive);
    delete g;
}

TEST(resting_open_read_does_not_block_descent) {
    Rig* g = upRig();
    g->setRotaryAngleFromCentre(150); g->boot();
    g->req(JobKind::RotaryHome); g->runUntilIdle(120000);
    CHECK(g->snap().lastResult == JobResult::Done);
    g->setRotaryAngleFromCentre(5.0); g->runMs(500);                // in the band, open at rest (held past the 400 ms rest filter)
    CHECK(!g->snap().homeSwitch);
    CHECK(g->sup.rotary().confidence == HomeConfidence::Latched);
    g->seek(0, 700); g->runUntilIdle(30000);
    CHECK(g->snap().lastResult == JobResult::DoneLimit || g->snap().lastResult == JobResult::DoneTarget);
    CHECK(g->snap().lifterTicks <= 5);
    CHECK_EQ(g->count(Ev::SeekClamped), 0);
    delete g;
}

// ---------------------------------------------------------------- absolute / relative

TEST(absolute_move_lands_within_tolerance_and_takes_shortest_path) {
    Rig* g = upRig();
    g->setRotaryAngleFromCentre(0.5); g->boot();                     // SwitchLive at boot: referenced
    CHECK(g->sup.rotary().confidence == HomeConfidence::SwitchLive);
    g->req(JobKind::RotaryAbsolute, 90, 500, 0); g->runUntilIdle(60000);
    CHECK(g->snap().lastResult == JobResult::DoneTarget || g->snap().lastResult == JobResult::Done);
    CHECK(circularDistance(g->snap().rotaryDeg, 90) <= 6);
    double p1 = g->hal.rotary.position();
    g->req(JobKind::RotaryAbsolute, 350, 500, 0); g->runUntilIdle(60000);   // shortest path: -100 deg, not +260
    CHECK(circularDistance(g->snap().rotaryDeg, 350) <= 6);
    CHECK(g->hal.rotary.position() < p1);
    delete g;
}

TEST(pd_720_and_minus_720_rotate_two_full_revolutions_and_speed_is_honoured) {
    Rig* g = upRig();
    g->setRotaryAngleFromCentre(0.5); g->boot();
    double p0 = g->hal.rotary.position();
    Ms t0 = g->hal.nowMs();
    g->req(JobKind::RotaryRelative, 720, 500, 0); g->runUntilIdle(120000);
    Ms slow = elapsed(g->hal.nowMs(), t0);
    CHECK(g->snap().lastResult == JobResult::DoneTarget || g->snap().lastResult == JobResult::Done);
    CHECK(std::fabs((g->hal.rotary.position() - p0) - 2 * 1184.6) < 40);
    t0 = g->hal.nowMs();
    g->req(JobKind::RotaryRelative, -720, 900, 0); g->runUntilIdle(120000);
    Ms fast = elapsed(g->hal.nowMs(), t0);
    CHECK(std::fabs(g->hal.rotary.position() - p0) < 60);
    CHECK(fast < slow);
    CHECK(circularDistance(g->snap().rotaryDeg, 0) <= 12);           // frame still valid after 4 revs (two 5-degree landings)
    delete g;
}

TEST(absolute_on_unreferenced_head_homes_first) {
    Rig* g = upRig();
    g->setRotaryAngleFromCentre(400); g->boot();
    CHECK(g->sup.rotary().confidence == HomeConfidence::Unknown);
    g->req(JobKind::RotaryAbsolute, 180, 500, 0); g->runUntilIdle(120000);
    int homes = 0, abs = 0;
    for (auto& e : g->log) if (e.code == Ev::JobStart) { if (e.a == (int)JobKind::RotaryHome) ++homes; if (e.a == (int)JobKind::RotaryAbsolute) ++abs; }
    CHECK_EQ(homes, 1); CHECK_EQ(abs, 1);
    CHECK(g->sup.rotary().confidence == HomeConfidence::Latched);
    CHECK(circularDistance(g->snap().rotaryDeg, 180) <= 6);
    delete g;
}

TEST(rotary_request_below_safe_height_auto_raises_then_rotates) {
    Rig g(gregConfig(false)); g.hal.lifter.setPosition(100); g.hal.rotary.setPosition(g.hal.rotary.params().camCentreTicks); g.boot();
    g.seek(450, 0, true); g.runUntilIdle(20000);                     // reference the lifter
    g.seek(100, 700); g.runUntilIdle(20000);
    CHECK(g.snap().lifterTicks < 225);
    g.req(JobKind::RotaryRelative, 90, 500, 0); g.runUntilIdle(120000);
    int raises = 0; for (auto& e : g.log) if (e.code == Ev::JobStart && e.a == (int)JobKind::LifterSeek && e.b == 450) ++raises;
    CHECK_EQ(raises, 2);                                              // the explicit one plus the auto-raise
    CHECK_EQ(g.snap().lifterTicks, 450);
    CHECK(g.snap().lastResult == JobResult::DoneTarget || g.snap().lastResult == JobResult::Done);
    CHECK_EQ(g.count(Ev::Refused), 0);
}

// ---------------------------------------------------------------- spin / jog

TEST(spin_runs_until_preempted_and_stalls_when_jammed) {
    Rig* g = upRig(); g->boot();
    g->req(JobKind::RotarySpin, 500); g->runMs(1500);
    CHECK(g->sup.activeJob() == JobKind::RotarySpin);
    double p = g->hal.rotary.position(); CHECK(p > 100);
    g->req(JobKind::RotarySpin, -500); g->runMs(50);                  // pre-empts the first spin
    CHECK(g->sup.activeJob() == JobKind::RotarySpin);
    g->runMs(1500);
    CHECK(g->hal.rotary.position() < p);
    g->hal.rotary.setJammed(true); g->runMs(1000);
    CHECK(g->sup.activeJob() == JobKind::None);
    CHECK(g->snap().lastResult == JobResult::Stalled);
    delete g;
}

TEST(jog_floors_at_12_percent_and_needs_override_below_safe_height) {
    Rig g(gregConfig(false)); g.hal.lifter.setPosition(100); g.hal.rotary.setPosition(500); g.boot();
    g.seek(450, 0, true); g.runUntilIdle(20000); g.seek(100, 700); g.runUntilIdle(20000);
    g.queue.jogKeepalive(g.hal.nowMs());
    g.req(JobKind::RotaryJog, 50); g.runMs(20);
    const Event* e = g.last(Ev::Refused); CHECK(e != nullptr); if (e) CHECK_EQ(e->b, (int)Reason::BelowSafeHeight);
    g.queue.setOverride(g.hal.nowMs() + CommandQueue::kOverrideMs);
    g.queue.jogKeepalive(g.hal.nowMs());
    g.req(JobKind::RotaryJog, 50); g.runMs(30);
    CHECK(g.sup.activeJob() == JobKind::RotaryJog);
    CHECK_EQ(g.hal.command(Axis::Rotary).duty, 120);                  // floored at 12 %
    g.runMs(1000);                                                    // 12 % does not turn this rotor: since 2026-10-03 the
    CHECK(g.sup.activeJob() == JobKind::None);                        // jog stops itself instead of pushing for the whole lease
    CHECK(g.snap().lastResult == JobResult::Stalled);
    g.queue.jogKeepalive(g.hal.nowMs());                              // at a power that turns it, the lease ends it
    g.req(JobKind::RotaryJog, 500); g.runMs(1500);
    CHECK(g.sup.activeJob() == JobKind::RotaryJog);
    g.runMs(1000);
    CHECK(g.sup.activeJob() == JobKind::None);
    CHECK(g.snap().lastReason == (uint8_t)Reason::LeaseExpired);
}

// ---------------------------------------------------------------- characterisation

TEST(characterise_measures_ticks_per_rev_and_writes_back_when_confident) {
    Rig* g = upRig();
    MotionConfig c = gregConfig(false); c.rotary.ticksPerRev = 1191.f; c.rotary.confident = false;   // the stale v3 number
    g->sup.setConfig(c);
    g->setRotaryAngleFromCentre(300); g->boot();
    g->req(JobKind::RotaryCharacterise, 3); g->runUntilIdle(120000);
    CHECK(g->snap().lastResult == JobResult::Done);
    const CharacteriseResult& r = g->sup.lastCharacterisation();
    CHECK_EQ(r.revs, 3);
    CHECK_EQ(r.detections, 4);
    CHECK(r.closures > r.detections);                                 // chatter counted, not mistaken for revolutions
    CHECK(std::fabs(r.meanTpr - 1184.6f) < 6.f);
    CHECK(r.spreadPct < 2.f);
    CHECK(r.confident);
    CHECK(std::fabs(g->sup.config().rotary.ticksPerRev - 1184.6f) < 6.f);
    CHECK(g->sup.takeCharacterisationUpdated());
    CHECK(g->sup.rotary().confidence == HomeConfidence::Latched);
    CHECK(absl(g->homeErrorTicks()) <= 8);
    delete g;
}

TEST(characterise_reports_no_switch_and_writes_nothing) {
    Rig* g = upRig();
    g->hal.rotary.setSwitchDisconnected(true); g->boot();
    g->req(JobKind::RotaryCharacterise, 2); g->runUntilIdle(120000);
    CHECK(g->snap().lastResult == JobResult::Failed);
    CHECK(g->snap().lastReason == (uint8_t)Reason::NoEdge);
    CHECK(!g->sup.takeCharacterisationUpdated());
    CHECK(std::fabs(g->sup.config().rotary.ticksPerRev - 1184.6f) < 0.01f);
    delete g;
}

TEST(home_parks_repeatably_on_the_leading_edge_with_drive_play) {
    // Todd's rotary (2026-09-20): play at the motor shaft. The park must land on the same spot
    // from either approach: the first closure met while creeping in along the sweep direction.
    double parked[2] = {0, 0}; int k = 0;
    for (int dir = -1; dir <= 1; dir += 2) {
        Rig g(gregConfig(false));
        g.hal.rotary.setParams([]{ sim::RotaryParams p; p.backlashTicks = 8.0; p.chatterP = 0.0; return p; }());
        g.hal.lifter.setPosition(450); g.hal.rotary.setPosition(g.hal.rotary.params().camCentreTicks + dir * 300); g.boot();
        g.req(JobKind::RotaryHome); g.runUntilIdle(40000);
        CHECK(g.snap().lastResult == JobResult::Done);
        CHECK(g.hal.rotary.distToCentre() <= g.hal.rotary.params().halfWidthMoving + 2.0);   // on the arc
        const Event* e = g.last(Ev::HomeSettle);
        CHECK(e && (e->b & 0xff) >= 3);                                                     // the wiggle ran
        parked[k++] = std::fmod(g.hal.rotary.position(), g.hal.rotary.params().ticksPerRev);   // same revolution frame
    }
    CHECK(std::fabs(parked[0] - parked[1]) <= 3.0);                                         // same spot from both sides
}


TEST(home_wiggles_and_parks_even_before_ticks_per_rev_is_known) {
    // Bench 2026-09-20: first :PA0 of a RAM-only boot (ticksPerRev = 0) skipped the wiggle.
    MotionConfig c = gregConfig(false); c.rotary.ticksPerRev = 0.f; c.rotary.confident = false;
    Rig g(c);
    g.hal.rotary.setParams([]{ sim::RotaryParams p; p.backlashTicks = 4.0; p.tauBrakeMs = 40.0; return p; }());
    g.hal.lifter.setPosition(450); g.hal.rotary.setPosition(g.hal.rotary.params().camCentreTicks); g.boot();
    g.req(JobKind::RotaryHome); g.runUntilIdle(40000);
    const Event* e = g.last(Ev::HomeSettle);
    CHECK(e && (e->b & 0xff) >= 1);                          // the wiggle ran (a heavy head may cut it short after the first swing)
    CHECK(g.hal.rotary.distToCentre() <= g.hal.rotary.params().halfWidthMoving + 2.0);
}

TEST(home_offset_parks_the_head_where_it_looks_straight_and_moves_measure_from_there) {
    // Todd, 2026-09-20: v3 parked on the arc's leading edge, so his head looks straight ~6 deg
    // from the arc centre. #PHOMEOFFSET moves the parking spot; "home" and :PA angles follow it.
    MotionConfig c = gregConfig(false); c.rotary.homeOffsetTicks = -20;
    Rig g(c);
    g.hal.rotary.setParams([]{ sim::RotaryParams p; p.backlashTicks = 4.0; return p; }());
    g.hal.lifter.setPosition(450); g.hal.rotary.setPosition(g.hal.rotary.params().camCentreTicks + 250); g.boot();
    g.req(JobKind::RotaryHome); g.runUntilIdle(40000);
    CHECK(g.snap().lastResult == JobResult::Done);
    CHECK(absl(g.snap().rotaryTicks - (-20)) <= 6);                     // parked at the offset, in the switch frame
    CHECK_EQ(g.snap().rotaryDeg, 0);                                    // which reads as 0 degrees
    g.req(JobKind::SafeLower); g.runUntilIdle(40000);                   // and counts as home for the descent
    CHECK(g.snap().lastResult == JobResult::DoneLimit);
    CHECK_EQ(g.count(Ev::SeekClamped), 0);
    g.seek(450, 0, true); g.runUntilIdle(20000);
    g.req(JobKind::RotaryAbsolute, 90, 0, 0); g.runUntilIdle(30000);    // 90 deg from the visual home
    Ticks expect = -20 + static_cast<Ticks>(1184.6f * 90 / 360 + 0.5f);
    CHECK(absl(g.snap().rotaryTicks - expect) <= 20);
    CHECK(absl(g.snap().rotaryDeg - 90) <= 6);
}

TEST(small_relative_moves_walk_in_single_ticks_and_land) {
    // Bench 2026-09-20: ":PD2" reported OVERSHOOT without moving, because a continuous move
    // brakes 16 ticks out. Moves under ~6 degrees now step one tick at a time.
    for (int lash = 0; lash <= 6; lash += 6) {
        Rig g(gregConfig(false));
        g.hal.rotary.setParams([lash]{ sim::RotaryParams p; p.backlashTicks = lash; return p; }());
        g.hal.lifter.setPosition(450); g.hal.rotary.setPosition(500); g.boot(); g.runMs(5);
        Ticks start = g.snap().rotaryTicks;
        g.req(JobKind::RotaryRelative, 2, 0, 0); g.runUntilIdle(20000);              // 2 deg = 7 ticks
        CHECK(g.snap().lastResult == JobResult::DoneTarget);
        CHECK(absl(g.snap().rotaryTicks - (start + 7)) <= 2);
        g.req(JobKind::RotaryRelative, -2, 0, 0); g.runUntilIdle(20000);
        CHECK(absl(g.snap().rotaryTicks - start) <= 3);
    }
}

TEST(home_parks_on_the_arc_even_when_the_encoder_slips_under_pulses) {
    // Todd, 2026-09-20: short pulses turn the count without the head (or the head without the
    // count). The park pass must land on the arc using the switch alone.
    Rig g(gregConfig(false));
    g.hal.rotary.setParams([]{ sim::RotaryParams p; p.phantomPerMsDriving = 0.15; p.backlashTicks = 6.0; return p; }());
    g.hal.lifter.setPosition(450); g.hal.rotary.setPosition(g.hal.rotary.params().camCentreTicks + 200); g.boot();
    g.req(JobKind::RotaryHome); g.runUntilIdle(40000);
    CHECK(g.snap().lastResult == JobResult::Done);
    CHECK(g.hal.rotary.distToCentre() <= g.hal.rotary.params().halfWidthMoving + 2.0);
}


