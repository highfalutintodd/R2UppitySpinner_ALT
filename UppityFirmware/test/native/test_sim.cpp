#include "uppity_test.h"
#include "../../src/sim/SimHal.h"

using namespace uppity;
using namespace uppity::sim;

static BridgeCommand drive(int duty) { BridgeCommand c; c.mode = Bridge::Drive; c.duty = static_cast<Throttle>(duty); return c; }
static BridgeCommand brake() { BridgeCommand c; c.mode = Bridge::Brake; return c; }

static LifterParams greg() { LifterParams p; p.breakawayDuty = 0.45; p.travelTicks = 450; p.kv = 0.5; return p; }
static LifterParams p19()  { LifterParams p; p.breakawayDuty = 0.40; p.travelTicks = 1350; p.kv = 0.3; return p; }

// ---------------------------------------------------------------- lifter model

TEST(sim_lifter_continuous_drive_reaches_top_and_hits_fast) {
    LifterModel m(greg());
    for (int t = 0; t < 4000 && !m.topLimit(); ++t) m.step(drive(650), true);
    CHECK(m.topLimit());
    CHECK(m.impactVelocityTop() > 0.20);             // most of full speed at the switch
    CHECK(m.encoder() >= 450);
}

TEST(sim_lifter_pulsed_cruise_hits_the_limit_slowly) {
    LifterModel m(greg());
    // breakaway burst, then 3 on / 1 off at 65 % — the v3 Greg pattern
    for (int t = 0; t < 150; ++t) m.step(drive(650), true);
    int t = 0;
    while (!m.topLimit() && t < 20000) { m.step((t % 4) < 3 ? drive(650) : brake(), true); ++t; }
    CHECK(m.topLimit());
    CHECK(m.impactVelocityTop() < 0.5 * 0.5);        // well under continuous speed
    CHECK(m.impactVelocityTop() < 0.20);
}

TEST(sim_lifter_pulsing_from_rest_starves_a_19_to_1) {
    LifterModel m(p19());
    // 5 on / 2 off at 45 %: window-averaged 32 %, below the 40 % breakaway → buzzes, no motion
    for (int t = 0; t < 600; ++t) m.step((t % 7) < 5 ? drive(450) : brake(), true);
    CHECK(!m.moving());
    CHECK_EQ(m.encoder(), 0);
    // continuous 45 % breaks it free, and then the same pulsing keeps it moving
    for (int t = 0; t < 200; ++t) m.step(drive(450), true);
    CHECK(m.moving());
    Ticks before = m.encoder();
    for (int t = 0; t < 600; ++t) m.step((t % 7) < 5 ? drive(450) : brake(), true);
    CHECK(m.encoder() > before + 5);
}

TEST(sim_lifter_brake_stops_within_milliseconds_and_screw_holds) {
    LifterModel m(greg());
    for (int t = 0; t < 400; ++t) m.step(drive(800), true);
    CHECK(m.velocity() > 0.3);
    for (int t = 0; t < 12; ++t) m.step(brake(), true);
    CHECK(m.velocity() < 0.01);
    Ticks held = m.encoder();
    for (int t = 0; t < 500; ++t) m.step(brake(), false);   // drivers off: nothing moves
    CHECK_EQ(m.encoder(), held);
}

TEST(sim_lifter_hard_stop_freezes_encoder_and_counts_hits) {
    LifterModel m(greg());
    for (int t = 0; t < 3000; ++t) m.step(drive(800), true);
    Ticks e = m.encoder();
    for (int t = 0; t < 200; ++t) m.step(drive(800), true);
    CHECK_EQ(m.encoder(), e);
    CHECK(m.hardStopHits() >= 1);
    CHECK(m.topLimit());
}

TEST(sim_lifter_faults_inject) {
    LifterModel m(greg());
    m.setDiagFor(3); CHECK(m.diag()); for (int i = 0; i < 3; ++i) m.step(brake(), true); CHECK(!m.diag());
    m.setJammed(true); for (int t = 0; t < 500; ++t) m.step(drive(900), true); CHECK_EQ(m.encoder(), 0);
    m.setJammed(false);
    LifterParams n = greg(); n.phantomPerMsIdle = 0.5; LifterModel noisy(n, 7);
    for (int t = 0; t < 200; ++t) noisy.step(brake(), true);
    CHECK(noisy.encoder() != 0);                    // phantom ticks while idle
}

// ---------------------------------------------------------------- rotary model

TEST(sim_rotary_switch_fires_once_per_rev_with_chatter) {
    RotaryModel m;
    int detections = 0; bool prev = false; double lastRev = -1;
    int revsSeen = 0; double startPos = m.position();
    for (int t = 0; t < 30000; ++t) {
        m.step(drive(500), true);
        bool sw = m.homeSwitch();
        if (sw && !prev) {
            double rev = std::floor((m.position() - startPos) / m.params().ticksPerRev);
            if (rev != lastRev) { ++detections; lastRev = rev; }
        }
        prev = sw;
        revsSeen = static_cast<int>((m.position() - startPos) / m.params().ticksPerRev);
        if (revsSeen >= 3) break;
    }
    CHECK(revsSeen >= 3);
    CHECK_EQ(detections, 3);
    CHECK(m.closures() > 3u * 2u);                   // chatter: several closures per detection
    CHECK(m.closures() < 3u * 40u);
}

TEST(sim_rotary_rest_reads_closed_only_in_the_core) {
    RotaryModel m;
    m.setPosition(m.params().camCentreTicks + 1.0);   // inside the 2-tick core
    m.step(brake(), true);
    CHECK(m.homeSwitch());
    m.setPosition(m.params().camCentreTicks + 5.0);   // inside the moving band, at rest
    m.step(brake(), true);
    CHECK(!m.homeSwitch());
    m.setPosition(m.params().camCentreTicks + 50.0);
    m.step(brake(), true);
    CHECK(!m.homeSwitch());
}

TEST(sim_rotary_short_pulse_moves_nothing_long_pulse_moves_a_burst) {
    RotaryModel m;
    for (int t = 0; t < 4; ++t) m.step(drive(500), true);
    for (int t = 0; t < 50; ++t) m.step(brake(), true);
    CHECK_EQ(m.encoder(), 0);
    for (int t = 0; t < 40; ++t) m.step(drive(500), true);
    for (int t = 0; t < 50; ++t) m.step(brake(), true);
    CHECK(m.encoder() >= 3);
    CHECK(m.encoder() <= 25);
}

TEST(sim_rotary_first_contact_edge_depends_on_direction) {
    RotaryModel a, b;
    double c = a.params().camCentreTicks;
    a.setPosition(c - 60); b.setPosition(c + 60);
    double firstA = -1, firstB = -1;
    for (int t = 0; t < 2000 && firstA < 0; ++t) { a.step(drive(400), true); if (a.homeSwitch()) firstA = a.position(); }
    for (int t = 0; t < 2000 && firstB < 0; ++t) { b.step(drive(-400), true); if (b.homeSwitch()) firstB = b.position(); }
    CHECK(firstA > 0 && firstB > 0);
    CHECK(firstA < c);                                // approaching upward: contact before centre
    CHECK(firstB > c);                                // approaching downward: contact after centre
    CHECK(firstB - firstA > 4);                       // the hysteresis a naive stop-on-contact home would carry
}

TEST(sim_rotary_switch_faults) {
    RotaryModel m;
    m.setSwitchDisconnected(true); m.setPosition(m.params().camCentreTicks); m.step(brake(), true); CHECK(!m.homeSwitch());
    m.setSwitchDisconnected(false); m.setSwitchStuckClosed(true); m.setPosition(500); m.step(brake(), true); CHECK(m.homeSwitch());
    m.setSwitchStuckClosed(false); m.setSwitchPolarityWrong(true); m.step(brake(), true); CHECK(m.homeSwitch());
}

// ---------------------------------------------------------------- SimHal

TEST(simhal_enable_latency_and_drive_gating) {
    SimHal h;
    h.requestEnable(true);
    CHECK(!h.enableAcked());
    h.advance(h.enableLatencyMs);
    CHECK(h.enableAcked());
    CHECK_EQ(h.enableWrites(), 1u);
    h.requestEnable(true);                            // unchanged: no extra write
    CHECK_EQ(h.enableWrites(), 1u);
    SimHal off; off.failEnable = true; off.requestEnable(true); off.advance(50);
    off.writeBridge(Axis::Lifter, drive(800)); off.advance(500);
    RawInputs in; off.readInputs(in);
    CHECK_EQ(in.lifterTicks, 0);                      // drivers never enabled: nothing moves
    CHECK(!in.enableAcked);
}

TEST(simhal_panic_stop_brakes_both_axes_and_is_recorded) {
    SimHal h; h.requestEnable(true); h.advance(10);
    h.writeBridge(Axis::Lifter, drive(700)); h.writeBridge(Axis::Rotary, drive(-500));
    h.advance(300);
    RawInputs in; h.readInputs(in);
    CHECK(in.lifterTicks > 0); CHECK(in.rotaryTicks < 0);
    h.panicStop();
    CHECK(h.command(Axis::Lifter).mode == Bridge::Brake);
    CHECK(h.command(Axis::Rotary).mode == Bridge::Brake);
    CHECK_EQ(h.panicCount(), 1u);
    CHECK_EQ(h.lastPanicAt(), 310u);
    const auto& last = h.rec(h.recordCount() - 1);
    CHECK(last.c.mode == Bridge::Brake);
    CHECK_EQ(last.t, 310u);
}

TEST(simhal_set_encoder_rereferences_and_invert_swaps_sense) {
    SimHal h; h.requestEnable(true); h.advance(10);
    h.writeBridge(Axis::Lifter, drive(700)); h.advance(200); h.writeBridge(Axis::Lifter, brake()); h.advance(20);
    RawInputs in; h.readInputs(in);
    CHECK(in.lifterTicks > 0);
    h.setEncoder(Axis::Lifter, 1350);
    h.readInputs(in); CHECK_EQ(in.lifterTicks, 1350);

    // A build whose mechanism runs backwards: the encoder follows the shaft, the mast does not.
    SimHal inv; inv.lifter.setParams([]{ LifterParams p; p.mastReversed = true; return p; }());
    inv.lifter.setPosition(600);
    inv.requestEnable(true); inv.advance(10);
    inv.setEncoder(Axis::Lifter, 0);                  // reference the count where we start
    inv.writeBridge(Axis::Lifter, drive(700)); inv.advance(200);
    inv.readInputs(in);
    CHECK(in.lifterTicks > 0);                        // encoder counts "up"...
    CHECK(inv.lifter.position() < 590);               // ...while the mast went DOWN: the builder's symptom
    inv.writeBridge(Axis::Lifter, brake()); inv.advance(20);
    double before = inv.lifter.position();
    inv.setLifterInverted(true);                      // the firmware's invert setting: negate drive AND count
    inv.setEncoder(Axis::Lifter, 0);
    inv.writeBridge(Axis::Lifter, drive(700)); inv.advance(200);
    inv.readInputs(in);
    CHECK(in.lifterTicks > 0);                        // now the count and the mast agree
    CHECK(inv.lifter.position() > before + 10);
}
