// Phase 4: the rescue tools (override, jog lease, nudge, set home here) and the policy changes
// that came with them (top limit is never "inside the opening"; alignment suspect after the
// override has let the head turn or descend unaligned).
#include "rig.h"
using namespace uppity;
using namespace uppity::rig;
using namespace uppity::sim;

// Lifter at the top, rotary homed then turned to about 90 degrees.
static void turnedAtTheTop(Rig& g, int deg = 90) {
    g.hal.lifter.setPosition(450); g.setRotaryAngleFromCentre(200); g.boot();
    g.req(JobKind::RotaryHome); g.runUntilIdle(60000);
    g.req(JobKind::RotaryAbsolute, deg, 0, 0); g.runUntilIdle(30000);
}
static int32_t lastRefusalReason(const Rig& g) { const Event* e = g.last(Ev::Refused); return e ? e->b : -1; }

// ---------------------------------------------------------------- queue

TEST(command_queue_jog_release_ends_the_lease_even_late_in_millis) {
    NoLock lock; CommandQueue q(lock);
    q.jogKeepalive(1000); CHECK(q.jogLeaseLive(1500));
    q.jogRelease(1600); CHECK(!q.jogLeaseLive(1600)); CHECK(!q.jogLeaseLive(1700));
    Ms late = 0xF0000000u;                                    // 46 days: 0 would read as live again here
    q.jogKeepalive(late); CHECK(q.jogLeaseLive(late + 10));
    q.jogRelease(late + 20); CHECK(!q.jogLeaseLive(late + 20)); CHECK(!q.jogLeaseLive(late + 1000));
}

// ---------------------------------------------------------------- policy

TEST(policy_the_top_switch_is_never_inside_the_opening) {
    // No safe-spin height set: only the top counts, so the policy's threshold IS the travel. A head
    // turned at the top with the count at exactly the travel used to pass as "inside the opening".
    PolicyContext c; c.safeSpinTicks = 450; c.ticksPerRev = 1184.6f;
    c.lifter.confidence = LifterConfidence::Known; c.lifter.ticks = 450; c.topLimit = true;
    c.rotary.confidence = HomeConfidence::Latched; c.rotary.ticks = 296;         // 90 degrees
    Decision d = checkLifterSeek(c, 450, 0);
    CHECK(d.verdict == Verdict::Clamp); CHECK_EQ(d.value, 450); CHECK(d.reason == Reason::RotaryNotHome);
    CHECK_EQ(checkLifterCreep(c, -1).value, 450);
    c.rotary.ticks = 0; CHECK(checkLifterSeek(c, 450, 0).verdict == Verdict::Allow);   // home: fine
}

TEST(policy_alignment_suspect_turns_off_the_inside_the_opening_exemption) {
    PolicyContext c; c.safeSpinTicks = 225; c.ticksPerRev = 1184.6f;
    c.lifter.confidence = LifterConfidence::Known; c.lifter.ticks = 100;
    c.rotary.confidence = HomeConfidence::Unknown;
    CHECK(checkLifterSeek(c, 100, 0).verdict == Verdict::Allow);                   // below safe height: aligned by construction
    CHECK(checkLifterCreep(c, -1).ok());
    c.alignmentSuspect = true;                                                     // ...unless the override let it get there turned
    Decision d = checkLifterSeek(c, 100, 0); CHECK(d.verdict == Verdict::Refuse); CHECK(d.reason == Reason::RotaryNotHome);
    d = checkLifterCreep(c, -1); CHECK(d.verdict == Verdict::Refuse); CHECK(d.reason == Reason::RotaryNotHome);
    CHECK(checkLifterSeek(c, 100, 300).ok());                                      // up is always fine
    c.overrideLive = true; CHECK(checkLifterSeek(c, 100, 0).verdict == Verdict::Allow);
    c.overrideLive = false; c.rotary.confidence = HomeConfidence::SwitchLive; CHECK(checkLifterSeek(c, 100, 0).verdict == Verdict::Allow);
    c.rotary.confidence = HomeConfidence::Unknown; c.lifter.ticks = 400;           // above: the ordinary clamp, suspect or not
    d = checkLifterSeek(c, 400, 0); CHECK(d.verdict == Verdict::Clamp); CHECK_EQ(d.value, 225);
}

// ---------------------------------------------------------------- supervisor: override

TEST(override_descent_with_a_turned_head_needs_a_home_before_the_next_descent) {
    Rig g(gregConfig(false)); turnedAtTheTop(g);
    CHECK(g.snap().rotaryDeg > 60 && g.snap().rotaryDeg < 120);
    g.queue.setOverride(g.hal.nowMs() + CommandQueue::kOverrideMs);
    g.seek(100, 650); g.runUntilIdle(20000);                                        // below safe height, head turned: only the override allows it
    CHECK(g.snap().lastResult == JobResult::DoneTarget);
    CHECK(g.sup.alignmentSuspect()); CHECK(g.snap().alignmentSuspect);
    const Event* a = g.last(Ev::Alignment); CHECK(a && a->a == 1);
    g.queue.setOverride(0); g.runMs(5);
    g.seek(0, 650); g.runUntilIdle(5000);                                          // used to pass as "inside the opening"
    CHECK(g.snap().lastResult == JobResult::Refused); CHECK_EQ(lastRefusalReason(g), (int32_t)Reason::RotaryNotHome);
    CHECK(g.snap().lifterTicks > 80);
    g.req(JobKind::RotaryHome); g.runUntilIdle(60000);                             // raises first (below safe), homes at the top
    g.runMs(5);
    CHECK(!g.sup.alignmentSuspect());
    a = g.last(Ev::Alignment); CHECK(a && a->a == 0);
    g.seek(0, 650, true); g.runUntilIdle(20000);
    CHECK(g.snap().lastResult == JobResult::DoneLimit);
}

TEST(override_rotary_jog_below_safe_height_runs_to_its_lease_and_marks_alignment) {
    // docs/REWRITE-PLAN.md §6 "Rescue override race": under the override a rotary jog below the
    // safe height must keep running through every policy evaluation until its lease ends.
    Rig g(gregConfig(false)); g.hal.lifter.setPosition(450); g.setRotaryAngleFromCentre(0); g.boot();
    g.req(JobKind::RotaryHome); g.runUntilIdle(60000);
    g.seek(100, 650); g.runUntilIdle(20000);                                        // head home: the descent is allowed
    CHECK(absl(g.snap().lifterTicks - 100) < 20);
    g.queue.setOverride(g.hal.nowMs() + CommandQueue::kOverrideMs);
    g.queue.jogKeepalive(g.hal.nowMs());
    g.req(JobKind::RotaryJog, 300); g.runMs(20);
    int running = 0;
    for (int i = 0; i < 3000; ++i) { if (i % 500 == 0) g.queue.jogKeepalive(g.hal.nowMs()); g.runMs(1); if (g.sup.activeJob() == JobKind::RotaryJog) ++running; }
    CHECK_EQ(running, 3000);                                                        // 3000 ticks, never stopped by the policy
    CHECK(g.sup.alignmentSuspect());
    g.runMs(2600);                                                                  // no more renewals: the lease ends it
    CHECK(g.sup.activeJob() == JobKind::None);
    CHECK(g.snap().lastReason == (uint8_t)Reason::LeaseExpired);
    g.queue.setOverride(0); g.runMs(5);
    g.seek(0, 650); g.runUntilIdle(3000);
    CHECK(g.snap().lastResult == JobResult::Refused);                               // turned inside the opening: home first
}

TEST(estop_ends_the_override_and_the_jog_lease_and_both_are_logged) {
    Rig g(gregConfig(false)); g.hal.lifter.setPosition(450); g.boot();
    g.queue.setOverride(g.hal.nowMs() + CommandQueue::kOverrideMs);
    g.runMs(3);
    const Event* o = g.last(Ev::Override); CHECK(o && o->a == 1 && o->b > 59000);
    CHECK_EQ(g.snap().overrideRemainingMs / 1000, 59u);
    g.queue.jogKeepalive(g.hal.nowMs());
    g.req(JobKind::RotaryJog, 300); g.runMs(30);
    CHECK(g.sup.activeJob() == JobKind::RotaryJog);
    g.estop.trip(EstopLatch::Source::Web, g.hal.nowMs()); g.runMs(3);
    CHECK(!g.queue.overrideLive(g.hal.nowMs())); CHECK(!g.queue.jogLeaseLive(g.hal.nowMs()));
    o = g.last(Ev::Override); CHECK(o && o->a == 0);
    g.estop.release(g.hal.nowMs()); g.runMs(5);
    CHECK_EQ(g.snap().overrideRemainingMs, 0u);                                     // a reset does not bring it back
}

TEST(override_expires_by_itself_after_sixty_seconds) {
    Rig g(gregConfig(false)); g.hal.lifter.setPosition(450); g.boot();
    g.queue.setOverride(g.hal.nowMs() + CommandQueue::kOverrideMs); g.runMs(2);
    int on = g.count(Ev::Override);
    g.runMs(59000); CHECK(g.snap().overrideRemainingMs > 0); CHECK_EQ(g.count(Ev::Override), on);
    g.runMs(1100); CHECK_EQ(g.snap().overrideRemainingMs, 0u);
    const Event* o = g.last(Ev::Override); CHECK(o && o->a == 0);
}

// ---------------------------------------------------------------- supervisor: set home here

TEST(set_rotary_home_here_makes_the_current_position_home) {
    MotionConfig c = gregConfig(false); c.rotary.homeOffsetTicks = -12;
    Rig g(c); g.hal.lifter.setPosition(450); g.setRotaryAngleFromCentre(250); g.boot();
    CHECK(g.snap().homeConf == HomeConfidence::Unknown);
    g.sup.requestSetRotaryHome(); g.runMs(2);
    CHECK(g.snap().homeConf == HomeConfidence::Latched); CHECK(g.snap().homeProv == HomeProvenance::Manual);
    CHECK_EQ(g.snap().rotaryTicks, -12); CHECK_EQ(g.snap().rotaryDeg, 0);          // the visual home is where 0 degrees sits
    const Event* e = g.last(Ev::HomeManual); CHECK(e && e->c == 0);
    // refused while something runs, and while the E-STOP is latched
    g.req(JobKind::RotarySpin, 500); g.runMs(50);
    g.sup.requestSetRotaryHome(); g.runMs(2);
    e = g.last(Ev::HomeManual); CHECK(e && e->c == (int32_t)Reason::Busy);
    g.estop.trip(EstopLatch::Source::Test, g.hal.nowMs()); g.runMs(3);
    Ticks before = g.snap().rotaryTicks;
    g.sup.requestSetRotaryHome(); g.runMs(2);
    e = g.last(Ev::HomeManual); CHECK(e && e->c == (int32_t)Reason::Estop);
    CHECK_EQ(g.snap().rotaryTicks, before);
}

// ---------------------------------------------------------------- lifter creep: wrong limit, step, powers

TEST(creep_stops_when_the_switch_it_is_leaving_closes_again) {
    // A reversed mechanism (v3 "inverted") with the firmware not told: from mid-travel the height
    // is unknown, so only "up" is allowed, and "up" drives the mast down onto the bottom switch.
    Rig g(gregConfig());
    g.hal.lifter.setParams([]{ sim::LifterParams p; p.breakawayDuty = 0.45; p.travelTicks = 450; p.kv = 0.5; p.mastReversed = true; return p; }());
    g.hal.lifter.setPosition(200); g.boot();
    CHECK(g.snap().lifterConf == LifterConfidence::Unknown);
    g.creep(+1, 0, false); g.runUntilIdle(30000);
    CHECK(g.snap().lastResult == JobResult::Failed);
    CHECK(g.snap().lastReason == (uint8_t)Reason::WrongLimit);
    CHECK(g.hal.lifter.position() > -4);                                            // stopped at the switch, not ground into the stop
    CHECK(g.hal.lifter.hardStopHits() <= 1);
}

TEST(nudge_steps_a_bounded_distance_with_continuous_drive) {
    Rig g(gregConfig()); g.hal.lifter.setPosition(0); g.boot();
    g.seek(300, 650); g.runUntilIdle(20000);
    Ticks from = g.snap().lifterTicks;
    Ms t0 = g.hal.nowMs();
    g.req(JobKind::LifterCreep, -40, 0, 2); g.runUntilIdle(5000);
    CHECK(g.snap().lastResult == JobResult::DoneTarget);
    Ticks moved = from - g.snap().lifterTicks;
    CHECK(moved >= 40); CHECK(moved <= 60);
    const Event* s = g.last(Ev::JobStart); CHECK(s && s->a == (int32_t)JobKind::LifterCreep && s->b == -40);
    // continuous: no brake between the first drive and the stop
    Ms firstDrive = 0, lastDrive = 0;
    for (int i = 0; i < g.hal.recordCount(); ++i) { const auto& r = g.hal.rec(i); if (r.t >= t0 && r.a == Axis::Lifter && r.c.mode == Bridge::Drive && r.c.duty != 0) { if (!firstDrive) firstDrive = r.t; lastDrive = r.t; } }
    CHECK(firstDrive > 0);
    CHECK_EQ(g.brakesBetween(firstDrive + 1, lastDrive), 0);
    g.req(JobKind::LifterCreep, 40, 0, 2); g.runUntilIdle(5000);                    // and back up
    CHECK(g.snap().lastResult == JobResult::DoneTarget);
    CHECK(absl(g.snap().lifterTicks - from) <= 25);
    g.req(JobKind::LifterCreep, 500, 0, 2); g.runUntilIdle(5000);                   // clamped to the longest step
    CHECK(g.snap().lifterTicks - from <= MotionSupervisor::kMaxStepTicks + 40);
}

TEST(jog_and_nudge_default_to_the_slow_power) {
    MotionConfig c = gregConfig(); c.jogPowerDown = 520; c.jogPowerUp = 700;
    Rig g(c); g.hal.lifter.setPosition(0); g.boot();
    g.seek(300, 650); g.runUntilIdle(20000);
    g.queue.jogKeepalive(g.hal.nowMs());
    g.creep(-1, 0, true); g.runMs(40);
    CHECK_EQ(g.hal.command(Axis::Lifter).duty, -520);
    g.queue.jogRelease(g.hal.nowMs()); g.runUntilIdle(1000);
    CHECK(g.snap().lastReason == (uint8_t)Reason::LeaseExpired);
    g.req(JobKind::LifterCreep, 30, 0, 2); g.runMs(40);
    CHECK_EQ(g.hal.command(Axis::Lifter).duty, 700);
    g.runUntilIdle(3000);
    MotionConfig d = gregConfig();                                                  // not configured: the profile's default minimum
    Rig h(d); h.hal.lifter.setPosition(0); h.boot();
    h.req(JobKind::LifterCreep, 30, 0, 2); h.runMs(40);
    CHECK_EQ(h.hal.command(Axis::Lifter).duty, 650);
}

TEST(wizard_creep_stalls_after_the_profile_timeout) {
    Rig g(gregConfig()); g.hal.lifter.setPosition(200); g.boot();
    g.hal.lifter.setJammed(true);
    g.creep(+1, 0, false); g.runUntilIdle(10000);
    CHECK(g.snap().lastResult == JobResult::Stalled);
    const Event* s = nullptr; const Event* e = nullptr;
    for (auto& ev : g.log) { if (ev.code == Ev::JobStart && ev.a == (int32_t)JobKind::LifterCreep) s = &ev; if (ev.code == Ev::JobEnd && ev.a == (int32_t)JobKind::LifterCreep) e = &ev; }
    CHECK(s && e);
    if (s && e) { Ms took = e->tMs - s->tMs; CHECK(took >= 1990); CHECK(took <= 2100); }   // Greg profile: 2000 ms
}

// ---------------------------------------------------------------- the safe-spin measurement, at the policy level

TEST(measurement_lowering_with_the_head_turned_needs_the_override_every_time) {
    MotionConfig c = gregConfig(false); c.safeSpinTicks = 0;                        // commissioning step 6: only the top counts
    Rig g(c); turnedAtTheTop(g);
    CHECK(g.hal.lifter.topLimit());
    Ticks top = g.snap().lifterTicks;
    g.req(JobKind::LifterCreep, -30, 0, 2); g.runUntilIdle(3000);                  // no override: the top is not inside the opening
    CHECK(g.snap().lifterTicks >= top - 2);
    g.queue.setOverride(g.hal.nowMs() + CommandQueue::kOverrideMs);
    g.req(JobKind::LifterCreep, -30, 0, 2); g.runUntilIdle(3000);
    CHECK(g.snap().lastResult == JobResult::DoneTarget);
    CHECK(g.snap().lifterTicks < top - 25);
    CHECK(g.sup.alignmentSuspect());
    g.queue.setOverride(0); g.runMs(3);
    Ticks here = g.snap().lifterTicks;
    g.req(JobKind::LifterCreep, -30, 0, 2); g.runUntilIdle(3000);                  // the override ended: refused, not "inside the opening"
    CHECK(g.snap().lastResult == JobResult::Refused);
    CHECK_EQ(g.snap().lifterTicks, here);
    g.req(JobKind::LifterCreep, 30, 0, 2); g.runUntilIdle(3000);                   // up is always allowed
    CHECK(g.snap().lastResult == JobResult::DoneTarget);
}
