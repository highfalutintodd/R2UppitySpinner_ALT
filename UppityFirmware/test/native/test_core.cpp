#include "uppity_test.h"
#include "../../src/core/Units.h"
#include "../../src/core/CircularMath.h"
#include "../../src/core/Profiles.h"
#include "../../src/core/EstopLatch.h"
#include "../../src/core/StatusSnapshot.h"
#include "../../src/core/EventLog.h"
#include "../../src/core/CommandQueue.h"

using namespace uppity;

// ---------------------------------------------------------------- units / circular math

TEST(units_throttle_conversions) {
    CHECK_EQ(pctToThrottle(50), 500);
    CHECK_EQ(pctToThrottle(150), 1000);
    CHECK_EQ(pctToThrottle(-150), -1000);
    CHECK_EQ(throttleToPct(504), 50);
    CHECK_EQ(throttleToPct(-996), -100);
    CHECK_EQ(elapsed(5, 0xFFFFFFF0u), 21u);      // survives millis() wrap
}

TEST(circular_math) {
    CHECK_EQ(normalizeDeg(-30), 330);
    CHECK_EQ(normalizeDeg(725), 5);
    CHECK_EQ(signedCircularDelta(350, 10), 20);
    CHECK_EQ(signedCircularDelta(10, 350), -20);
    CHECK_EQ(signedCircularDelta(0, 180), 180);
    CHECK_EQ(circularDistance(90, 270), 180);
    CHECK(withinArc(350, 10, 5));
    CHECK(!withinArc(350, 10, 180));
    CHECK_EQ(signedTickDelta(1100, 50, 1191), 141);
    CHECK_EQ(signedTickDelta(50, 1100, 1191), -141);
    CHECK_EQ(modTicks(-5, 1191), 1186);
}

TEST(profiles_data_matches_v3) {
    const auto& g = profileFor(LifterMotor::Pololu4757_6p3);
    const auto& p19 = profileFor(LifterMotor::Pololu4751_19);
    const auto& ia = profileFor(LifterMotor::IAParts);
    CHECK_EQ(g.defaultMinPowerPct, 65); CHECK_EQ(g.pulseOnMs, 3); CHECK_EQ(g.pulseOffMs, 1); CHECK_EQ(g.stallTimeoutMs, 2000);
    CHECK_EQ(p19.maxThrottle, 750); CHECK_EQ(p19.calibrationSweepCapPct, 85); CHECK_EQ(p19.stallTimeoutMs, 700);
    CHECK_EQ(p19.pulseOnMs, 5); CHECK_EQ(p19.pulseOffMs, 2); CHECK(p19.softApproachEnabled); CHECK_EQ(p19.softApproachPct, 15);
    CHECK_EQ(ia.defaultTravelTicks, 845);
    CHECK_EQ(effectiveMaxThrottle(LifterMotor::Pololu4751_19, false), 750);
    CHECK_EQ(effectiveMaxThrottle(LifterMotor::Pololu4751_19, true), 1000);
    CHECK_EQ(effectiveMaxThrottle(LifterMotor::Pololu4757_6p3, true), 1000);   // expert is a no-op elsewhere
    CHECK_EQ(effectiveSweepCapPct(LifterMotor::Pololu4751_19, true), 100);
    RotaryCharacterisation rc; rc.ticksPerRev = 1191.f;
    CHECK_EQ(rc.degToTicks(360.f), 1191);
    CHECK_EQ(rc.degToTicks(-90.f), -298);
    CHECK_NEAR(rc.ticksToDeg(1191), 360.0, 0.001);
}

// ---------------------------------------------------------------- E-STOP latch

static int gPanics = 0;
static void panicHook(void*) { ++gPanics; }

TEST(estop_latch_is_one_way_and_records_first_source) {
    gPanics = 0;
    EstopLatch e; e.setPanic(panicHook, nullptr);
    CHECK(!e.tripped());
    e.trip(EstopLatch::Source::Web, 1000);
    CHECK(e.tripped());
    CHECK(e.source() == EstopLatch::Source::Web);
    CHECK_EQ(e.trippedAt(), 1000u);
    CHECK_EQ(gPanics, 1);
    e.trip(EstopLatch::Source::Mesh, 1500);           // second trip: still web, panic called again (harmless)
    CHECK(e.source() == EstopLatch::Source::Web);
    CHECK_EQ(gPanics, 2);
    CHECK_EQ(e.tripCount(), 2u);
    CHECK_EQ(e.seq(), 1u);
    CHECK(e.release(2000));
    CHECK(!e.tripped());
    CHECK(!e.release(2001));                          // releasing twice is a no-op
    e.trip(EstopLatch::Source::SerialCmd, 3000);
    CHECK(e.source() == EstopLatch::Source::SerialCmd);
    CHECK_EQ(e.seq(), 2u);
}

// ---------------------------------------------------------------- snapshot seqlock

TEST(snapshot_round_trip) {
    SnapshotPublisher pub;
    Snapshot s; s.tMs = 42; s.lifterTicks = 1350; s.rotaryDeg = 271; s.homeConf = HomeConfidence::Latched;
    pub.publish(s);
    Snapshot out;
    CHECK(pub.read(out));
    CHECK_EQ(out.tMs, 42u); CHECK_EQ(out.lifterTicks, 1350); CHECK_EQ(out.rotaryDeg, 271);
    CHECK(out.homeConf == HomeConfidence::Latched);
    CHECK_EQ(out.seq, 2u);
    s.tMs = 43; pub.publish(s);
    CHECK(pub.read(out)); CHECK_EQ(out.tMs, 43u); CHECK_EQ(out.seq, 4u);
}

TEST(snapshot_reads_before_first_publish_are_defaults) {
    SnapshotPublisher pub; Snapshot out; out.tMs = 99;
    CHECK(pub.read(out));
    CHECK_EQ(out.tMs, 0u);
    CHECK(out.state == MotionState::Locked);
}

// ---------------------------------------------------------------- event ring

TEST(event_ring_orders_and_counts_drops) {
    EventRing<4> r;
    for (int i = 0; i < 4; ++i) CHECK(r.push(Ev::Phase, i, 0, 0, i));
    CHECK(!r.push(Ev::Phase, 99, 0, 0, 99));
    CHECK_EQ(r.dropped(), 1u);
    CHECK_EQ(r.pending(), 4u);
    Event e;
    for (int i = 0; i < 4; ++i) { CHECK(r.pop(e)); CHECK_EQ(e.a, i); CHECK_EQ(e.tMs, (Ms)i); }
    CHECK(!r.pop(e));
    CHECK(r.push(Ev::Boot, 1, 2, 3, 7));
    CHECK(r.pop(e)); CHECK(e.code == Ev::Boot); CHECK_EQ(e.c, 3);
}

// ---------------------------------------------------------------- command queue

TEST(command_queue_ids_full_flush_and_leases) {
    NoLock lock; CommandQueue q(lock);
    MotionRequest r; r.kind = JobKind::LifterSeek; r.a = 1350; r.source = ReqSource::Web;
    uint32_t first = q.push(r);
    CHECK_EQ(first, 1u);
    for (int i = 0; i < CommandQueue::kDepth - 1; ++i) CHECK(q.push(r) != 0);
    CHECK_EQ(q.push(r), 0u);                          // full → Busy
    CHECK_EQ(q.size(), CommandQueue::kDepth);
    MotionRequest out;
    CHECK(q.pop(out)); CHECK_EQ(out.id, 1u); CHECK(out.kind == JobKind::LifterSeek); CHECK_EQ(out.a, 1350);
    q.flush(); CHECK_EQ(q.size(), 0); CHECK(!q.pop(out));

    CHECK(!q.takeCancel());
    q.cancel(); CHECK(q.takeCancel()); CHECK(!q.takeCancel());   // consumed once

    CHECK(!q.jogLeaseLive(1000));
    q.jogKeepalive(1000);
    CHECK(q.jogLeaseLive(1000 + CommandQueue::kJogLeaseMs - 1));
    CHECK(!q.jogLeaseLive(1000 + CommandQueue::kJogLeaseMs));

    CHECK(!q.overrideLive(5000));
    q.setOverride(5000 + CommandQueue::kOverrideMs);
    CHECK(q.overrideLive(5000));
    CHECK(q.overrideLive(5000 + CommandQueue::kOverrideMs - 1));
    CHECK(!q.overrideLive(5000 + CommandQueue::kOverrideMs));
    q.setOverride(0); CHECK(!q.overrideLive(5000));
    // across a millis() wrap
    q.setOverride(100); CHECK(q.overrideLive(0xFFFFFF00u)); CHECK(!q.overrideLive(200));
}
