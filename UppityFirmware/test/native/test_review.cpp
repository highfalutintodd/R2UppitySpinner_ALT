// Code review 2026-10 (docs/CODE-REVIEW-2026-10.md): one regression test per defect fixed. Each
// failed on the code as it stood at 019228e and passes with its fix. The finding's number in the
// review doc is in the test name.
#include "rig.h"
#include "../../src/core/Settings.h"
#include "../../src/core/Sequencer.h"
#include "../../src/core/RandomDirector.h"
#include "../../src/core/Dispatcher.h"
#include "../../src/core/Commissioning.h"
#include "../../src/core/LineAssembler.h"
#include <string>
using namespace uppity;
using namespace uppity::rig;
using namespace uppity::sim;

namespace {

// How far the simulated head really is from the switch centre, in degrees (0..180).
double headOffDeg(const Rig& g) {
    const auto& rp = g.hal.rotary.params();
    double a = std::fmod(g.hal.rotary.position() - rp.camCentreTicks, rp.ticksPerRev); if (a < 0) a += rp.ticksPerRev;
    if (a > rp.ticksPerRev / 2) a -= rp.ticksPerRev;
    return std::fabs(a) * 360.0 / rp.ticksPerRev;
}

// Todd's switch: the ball rides about 50 ticks of the cam while moving (BENCH-PHASE1: arc 50-53).
void toddArc(Rig& g) { RotaryParams rp = g.hal.rotary.params(); rp.halfWidthMoving = 25.0; g.hal.rotary.setParams(rp); }

void turnedAtTheTop(Rig& g, int deg = 90) {
    g.hal.lifter.setPosition(450); g.setRotaryAngleFromCentre(200); g.boot();
    g.req(JobKind::RotaryHome); g.runUntilIdle(60000);
    g.req(JobKind::RotaryAbsolute, deg, 0, 0); g.runUntilIdle(30000);
}

// Run until idle, recording the head's angle whenever the mast is below `below` (real height).
double worstTurnBelow(Rig& g, double below, int maxMs = 60000) {
    double worst = 0;
    for (int i = 0; i < maxMs; ++i) {
        g.runMs(1);
        if (g.hal.lifter.position() < below) worst = std::max(worst, headOffDeg(g));
        if (i > 100 && g.sup.activeJob() == JobKind::None && g.queue.size() == 0) break;
    }
    return worst;
}

// Spin until k ms after the switch (raw) opened behind the head; k = -1 stops while still on it.
void spinPastHome(Rig& g, int k) {
    bool seen = false; int since = -1;
    for (int i = 0; i < 20000; ++i) {
        g.runMs(1);
        bool on = g.hal.rotary.homeSwitch();
        if (on) { seen = true; since = -1; } else if (seen) since = since < 0 ? 0 : since + 1;
        if (seen && since == k) return;
    }
}

struct VecSink final : TextSink {
    std::vector<std::string> lines;
    void line(const char* t) override { lines.push_back(t); }
    int count(const char* n) const { int k = 0; for (auto& l : lines) if (l.find(n) != std::string::npos) ++k; return k; }
};

int jobStarts(const Rig& g, JobKind k, size_t from = 0) {
    int n = 0; for (size_t i = from; i < g.log.size(); ++i) if (g.log[i].code == Ev::JobStart && g.log[i].a == static_cast<int>(k)) ++n; return n;
}

} // namespace

// ---------------------------------------------------------------- 1: the frame taken from a moving head

// A spin interrupted just after the head passed the home switch: the switch's 30 ms release
// debounce still read closed, the rest-contact rule re-zeroed the frame on the coasting head, and
// the next :PH skipped its home and lowered the head 11-20 degrees off.
TEST(review01_a_spin_interrupted_past_home_does_not_move_the_frame) {
    int bad = 0, corrected = 0;
    for (int thr : {500, 1000}) for (int k = 0; k <= 32; k += 4) {
        Rig g(gregConfig(false)); toddArc(g);
        g.hal.lifter.setPosition(450); g.setRotaryAngleFromCentre(0); g.boot();
        g.req(JobKind::RotaryAbsolute, 90, 0, 0); g.runUntilIdle(30000); g.runMs(600);   // LATCHED, off home
        g.req(JobKind::RotarySpin, thr); spinPastHome(g, k);
        size_t mark = g.log.size();
        g.req(JobKind::SafeLower, 0, 700, 0);                                             // :PH interrupts the spin
        double worst = worstTurnBelow(g, 220);
        for (size_t i = mark; i < g.log.size(); ++i) if (g.log[i].code == Ev::HomeFrameCorrected) ++corrected;
        if (worst > 10.6) { ++bad; std::printf("  [review01 thr=%d k=%d] lowered %.1f deg off\n", thr, k, worst); }   // 10 degrees by the count; a tick of slack
    }
    CHECK_EQ(bad, 0);
    CHECK_EQ(corrected, 0);
}

TEST(review01_an_unreferenced_head_is_not_zeroed_while_it_coasts) {
    int bad = 0;
    for (int k = -1; k <= 31; k += 4) {
        Rig g(gregConfig(false)); toddArc(g);
        g.hal.lifter.setPosition(450); g.setRotaryAngleFromCentre(400); g.boot();      // UNKNOWN: off the switch at power-up
        g.req(JobKind::RotarySpin, 1000); spinPastHome(g, k);
        g.req(JobKind::SafeLower, 0, 700, 0);
        double worst = worstTurnBelow(g, 220);
        if (worst > 10.0) { ++bad; std::printf("  [review01 unknown k=%d] lowered %.1f deg off\n", k, worst); }
    }
    CHECK_EQ(bad, 0);
}

TEST(review01_an_estop_as_the_head_passes_home_does_not_move_the_frame) {
    int bad = 0;
    for (int k : {10, 16, 22, 28}) {
        Rig g(gregConfig(false)); toddArc(g);
        g.hal.lifter.setPosition(450); g.setRotaryAngleFromCentre(0); g.boot();
        g.req(JobKind::RotaryAbsolute, 90, 0, 0); g.runUntilIdle(30000); g.runMs(600);
        g.req(JobKind::RotarySpin, 1000); spinPastHome(g, k);
        g.estop.trip(EstopLatch::Source::Web, g.hal.nowMs()); g.runMs(3000);
        g.estop.release(g.hal.nowMs()); g.runMs(10);
        g.req(JobKind::SafeLower, 0, 700, 0);
        double worst = worstTurnBelow(g, 220);
        if (worst > 10.0) { ++bad; std::printf("  [review01 estop k=%d] lowered %.1f deg off\n", k, worst); }
    }
    CHECK_EQ(bad, 0);
}

// The fix must not cost the ordinary rest contact: a head parked on the switch is still trusted.
TEST(review01_a_head_resting_on_the_switch_still_counts_as_home) {
    Rig g(gregConfig(false)); g.hal.lifter.setPosition(450); g.setRotaryAngleFromCentre(0); g.boot();
    g.req(JobKind::RotaryRelative, 90, 0, 0); g.runUntilIdle(30000); g.runMs(600);    // LATCHED, off home
    g.setRotaryAngleFromCentre(0);                                                     // the head put back on the switch by hand
    g.runMs(1000);
    CHECK(g.hal.rotary.homeSwitch());
    CHECK(g.sup.rotary().confidence == HomeConfidence::SwitchLive);                    // trusted again at rest
    g.seek(0, 0, true); g.runUntilIdle(30000);
    CHECK(g.hal.lifter.position() < 5);
}

// ---------------------------------------------------------------- 2: a stall that starts on the switch

// The encoder stops counting while the head still turns (a slipping disc, a loose lead): the job
// ends Stalled. From LATCHED the frame went STALE; from SWITCH_LIVE (the parked state) it stayed
// trusted, became LATCHED on the frozen count, and :PH lowered the head ~90 degrees off.
TEST(review02_a_stall_from_rest_on_the_switch_makes_the_frame_stale) {
    Rig g(gregConfig(false)); g.hal.lifter.setPosition(450); g.setRotaryAngleFromCentre(0); g.boot();
    CHECK(g.sup.rotary().confidence == HomeConfidence::SwitchLive);
    g.req(JobKind::RotarySpin, 600); g.runMs(60);
    g.hal.rotary.setEncoderFrozen(true);
    g.runUntilIdle(5000); g.runMs(800);
    CHECK(g.snap().lastResult == JobResult::Stalled);
    CHECK(g.sup.rotary().confidence == HomeConfidence::Stale);
    g.hal.rotary.setEncoderFrozen(false);
    g.req(JobKind::SafeLower, 0, 700, 0);
    CHECK(worstTurnBelow(g, 220) <= 10.0);
}

// ---------------------------------------------------------------- 3: a home switch stuck closed

// A switch that reads closed everywhere (stuck microswitch, shorted lead, #PROTARYNC the wrong way
// round) made every resting angle "home": :PA90 then :PH lowered the head 85 degrees off.
TEST(review03_a_home_switch_stuck_closed_is_caught_while_the_head_turns) {
    Rig g(gregConfig(false)); g.hal.lifter.setPosition(450); g.setRotaryAngleFromCentre(0); g.boot();
    g.hal.rotary.setSwitchStuckClosed(true);
    g.req(JobKind::RotaryAbsolute, 90, 0, 0); g.runUntilIdle(30000); g.runMs(800);
    CHECK(g.snap().faults & F_HomeSwitchStuck);
    CHECK(g.sup.rotary().confidence == HomeConfidence::Stale);
    g.req(JobKind::SafeLower, 0, 700, 0);
    CHECK(worstTurnBelow(g, 220) <= 10.0);
    CHECK(g.hal.lifter.position() >= 220);                     // the home fails (switch stuck): stays raised
}

// ...and a working switch with Todd's 50-tick arc, spun hard, never trips it.
TEST(review03_a_working_switch_never_reads_as_stuck) {
    Rig g(gregConfig(false)); toddArc(g); g.hal.lifter.setPosition(450); g.setRotaryAngleFromCentre(0); g.boot();
    for (int thr : {300, 600, 1000, -1000}) { g.req(JobKind::RotarySpin, thr); g.runMs(4000); }
    g.req(JobKind::RotaryHome); g.runUntilIdle(60000);
    g.req(JobKind::RotaryAbsolute, 180, 0, 0); g.runUntilIdle(30000);
    g.req(JobKind::RotaryHome); g.runUntilIdle(60000);
    CHECK(!(g.snap().faults & F_HomeSwitchStuck));
}

// ---------------------------------------------------------------- 4: lease and override wrap-around

TEST(review04_the_override_and_the_jog_lease_do_not_come_back_after_24_days) {
    NoLock l; CommandQueue q(l);
    CHECK(!q.jogLeaseLive(0x80000000u + 5000));                 // never used, 24.9 days up
    q.setOverride(1000 + CommandQueue::kOverrideMs);
    q.expireLeases(1000 + CommandQueue::kOverrideMs + 1);       // the supervisor's tick, once it has run out
    CHECK(!q.overrideLive(1000 + CommandQueue::kOverrideMs + 0x80000000u + 5000));
    q.jogKeepalive(1000); q.jogRelease(1500);
    CHECK(!q.jogLeaseLive(1500 + 0x80000000u + 5000));
}

TEST(review04_an_expired_override_never_lets_a_turned_head_down_later) {
    Rig g(gregConfig(false)); turnedAtTheTop(g); g.runMs(600);
    g.queue.setOverride(g.hal.nowMs() + CommandQueue::kOverrideMs); g.runMs(61000);
    CHECK(!g.queue.overrideLive(g.hal.nowMs()));
    const Ms off = 0x80000000u;                                  // the board stays on for 24.9 days
    auto run = [&](int n) { for (int i = 0; i < n; ++i) { g.sup.tick(g.hal.nowMs() + off); g.hal.advance(1); g.drain(); } };
    run(5);
    CHECK_EQ(g.snap().overrideRemainingMs, 0u);
    g.seek(0, 650, true);
    double lowest = 1e9; for (int i = 0; i < 20000; ++i) { run(1); lowest = std::min(lowest, g.hal.lifter.position()); }
    CHECK(lowest >= 215);
}

// ---------------------------------------------------------------- 5: the override is for the operator

// The 60 s override lifted rule 2 for every request source: a ":PP0" from the dome's bus during
// a rescue lowered the turned head into the frame.
TEST(review05_the_override_does_not_let_a_bus_command_lower_a_turned_head) {
    Rig g(gregConfig(false)); turnedAtTheTop(g);
    CHECK(headOffDeg(g) > 60);
    g.queue.setOverride(g.hal.nowMs() + CommandQueue::kOverrideMs);
    MotionRequest r; r.kind = JobKind::LifterSeek; r.a = 0; r.c = 1; r.source = ReqSource::SerialCmd;
    g.queue.push(r); g.runUntilIdle(30000);
    CHECK(g.hal.lifter.position() >= 215);
    for (ReqSource s : {ReqSource::Mesh, ReqSource::Random, ReqSource::Trigger}) {
        r.source = s; g.queue.push(r); g.runUntilIdle(30000);
        CHECK(g.hal.lifter.position() >= 215);
    }
}

// ...while the operator's own nudge from the page still gets it (commissioning step 6, Rescue).
TEST(review05_the_override_still_works_for_the_page_and_the_usb_console) {
    for (ReqSource s : {ReqSource::Web, ReqSource::SerialUsb}) {
        Rig g(gregConfig(false)); turnedAtTheTop(g); g.runMs(600);
        g.seek(225, 650); g.runUntilIdle(30000); g.runMs(500);
        double before = g.hal.lifter.position();
        g.queue.setOverride(g.hal.nowMs() + CommandQueue::kOverrideMs);
        MotionRequest r; r.kind = JobKind::LifterCreep; r.a = -30; r.c = 2; r.source = s;
        g.queue.push(r); g.runUntilIdle(5000); g.runMs(500);
        CHECK(g.hal.lifter.position() < before - 15);
    }
}

// While commissioning ran, the lock was lifted for every source: a bus ":PP100" moved a board
// whose direction, rotary and travel were not yet known.
TEST(review05_commissioning_unlocks_only_its_own_moves_and_the_operator) {
    MotionConfig c = gregConfig(); c.commissioned = false; c.commissioningActive = true;
    Rig g(c); g.hal.lifter.setPosition(0); g.boot();
    MotionRequest r; r.kind = JobKind::LifterSeek; r.a = 450; r.c = 1; r.source = ReqSource::SerialCmd;
    g.queue.push(r); g.runUntilIdle(30000);
    CHECK(g.hal.lifter.position() < 5);
    CHECK(g.snap().lastReason == static_cast<uint8_t>(Reason::Locked));
    r.source = ReqSource::Commission; g.queue.push(r); g.runUntilIdle(30000);
    CHECK(g.hal.lifter.position() > 440);
}

// ...and random mode can only be started by someone watching while commissioning runs.
TEST(review05_a_bus_random_mode_is_refused_during_commissioning) {
    MotionConfig c = gregConfig(); c.commissioned = false; c.commissioningActive = true;
    Rig g(c); g.hal.lifter.setPosition(0); g.boot();
    MemSequenceStore store; XorShiftRandom rng{7}; VecSink out; Sequencer seq;
    struct L : Sequencer::Listener { int starts = 0; void onLight(int) override {} void onRandom(const Command&) override { ++starts; } void onRandomStop() override {} void onEstop() override {} } l;
    seq.begin(g.queue, g.pub, store, rng, out, l);
    Command els[Sequencer::kMaxElements]; char bad[32];
    int n = Sequencer::parseChain(":PM", els, Sequencer::kMaxElements, bad, sizeof bad);
    seq.submit(els, n, ReqSource::SerialCmd, false);
    CHECK_EQ(l.starts, 0);
    seq.submit(els, n, ReqSource::Web, false);
    CHECK_EQ(l.starts, 1);
}

// ---------------------------------------------------------------- 6: rotary switched off with the head turned

TEST(review06_switching_the_rotary_off_does_not_let_a_turned_head_down) {
    Rig g(gregConfig(false)); turnedAtTheTop(g);
    CHECK(headOffDeg(g) > 60);
    g.sup.setConfig(gregConfig(true)); g.runMs(5);               // "#PR" / "#PROTARYOFF1"
    g.seek(0, 650, true); g.runUntilIdle(30000);
    CHECK(g.hal.lifter.position() >= 215);
    CHECK(g.sup.headMayBeTurned());
}

// A board that powers up with the rotary off (no rotary fitted) lowers as before.
TEST(review06_a_board_without_a_rotary_lowers_freely) {
    Rig g(gregConfig(true)); g.hal.lifter.setPosition(450); g.boot();
    g.seek(0, 650, true); g.runUntilIdle(30000);
    CHECK(g.hal.lifter.position() < 5);
}

// ---------------------------------------------------------------- 7: safe-spin against a guessed travel

// The top switch sets the count to the travel in use. With only the profile's guess (no #PTRAVEL,
// no calibration: a factory reset followed by a settings restore), the count read ~200 high and the
// head turned well below the measured safe-spin height. Until the travel is measured or set, only
// the top counts as clear.
TEST(review07_safe_spin_waits_for_a_measured_travel) {
    MotionConfig c = gregConfig(false);
    c.travelTicks = 0; c.travelCalibrated = false;              // the 6.3:1 profile guesses 1350; the rig's lifter is 450
    c.safeSpinTicks = 300;
    Rig g(c); g.hal.lifter.setPosition(0); g.setRotaryAngleFromCentre(0); g.boot();
    g.seek(1350, 650, true); g.runUntilIdle(30000);             // :PP100: the top switch sets the count to 1350
    CHECK(g.hal.lifter.topLimit());
    g.seek(700, 650); g.runUntilIdle(30000);                    // count 700 (above 300) = real height well below 300
    double lowAtTurn = 1e9;
    g.req(JobKind::RotaryAbsolute, 90, 0, 0);
    for (int i = 0; i < 30000; ++i) { g.runMs(1); if (g.hal.rotary.moving()) lowAtTurn = std::min(lowAtTurn, g.hal.lifter.position()); if (i > 100 && g.sup.activeJob() == JobKind::None && g.queue.size() == 0) break; }
    CHECK(lowAtTurn >= 440);                                    // it raised to the top first
}

// ---------------------------------------------------------------- 8: the drift hold after an E-STOP

TEST(review08_an_estop_ends_the_drift_hold) {
    Rig g(gregConfig()); g.hal.lifter.setPosition(0); g.boot();
    g.seek(300, 650); g.runUntilIdle(20000);
    CHECK(g.sup.holdTarget() >= 0);
    g.hal.lifter.setPosition(200);
    for (int i = 0; i < 7000 && g.sup.activeJob() == JobKind::None; ++i) g.runMs(1);
    CHECK(g.sup.activeJob() == JobKind::LifterSeek);            // the hold's correction
    g.runMs(40);
    g.estop.trip(EstopLatch::Source::Web, g.hal.nowMs()); g.runMs(3000);
    g.estop.release(g.hal.nowMs());
    size_t mark = g.log.size(); g.runMs(8000);
    int starts = 0; for (size_t i = mark; i < g.log.size(); ++i) if (g.log[i].code == Ev::JobStart) ++starts;
    CHECK_EQ(starts, 0);
    CHECK(g.sup.holdTarget() < 0);
}

TEST(review08_inverting_the_lifter_ends_the_drift_hold) {
    Rig g(gregConfig()); g.hal.lifter.setPosition(0); g.boot();
    g.seek(300, 650); g.runUntilIdle(20000); g.runMs(3000);
    CHECK(g.sup.holdTarget() >= 0);
    MotionConfig c = gregConfig(); c.lifterInverted = true; g.sup.setConfig(c);
    size_t mark = g.log.size(); g.runMs(12000);
    int starts = 0; for (size_t i = mark; i < g.log.size(); ++i) if (g.log[i].code == Ev::JobStart) ++starts;
    CHECK_EQ(starts, 0);
}

// ---------------------------------------------------------------- 9: an E-STOP that lands mid-tick

namespace {
// A queue lock that trips the latch the moment the supervisor's tick takes it: the trip then lands
// after the tick sampled the latch and before it wrote its outputs, as a web E-STOP on core 0 can.
struct TripLock final : Lock {
    EstopLatch* latch = nullptr; bool armed = false; Ms at = 0; const sim::SimHal* hal = nullptr;
    void lock() override { if (armed && latch) { armed = false; at = hal->nowMs(); latch->trip(EstopLatch::Source::Web, at); } }
    void unlock() override {}
};
}
TEST(review09_an_estop_that_lands_mid_tick_is_honoured_before_the_outputs) {
    sim::SimHal hal; TripLock lock; CommandQueue queue{lock}; SnapshotPublisher pub; EventLog events; EstopLatch estop;
    estop.setPanic([](void* ctx) { static_cast<sim::SimHal*>(ctx)->panicStop(); }, &hal);
    sim::LifterParams p; p.breakawayDuty = 0.45; p.travelTicks = 450; p.kv = 0.5; hal.lifter.setParams(p);
    MotionSupervisor sup{hal, estop, queue, pub, events};
    sup.setConfig(gregConfig()); hal.lifter.setPosition(0);
    lock.latch = &estop; lock.hal = &hal;
    sup.begin(hal.nowMs());
    MotionRequest r; r.kind = JobKind::LifterSeek; r.a = 450; r.c = 1; r.source = ReqSource::Test; queue.push(r);
    Event e;
    for (int i = 0; i < 400; ++i) { sup.tick(hal.nowMs()); hal.advance(1); while (events.pop(e)) {} }
    CHECK(hal.command(Axis::Lifter).mode == Bridge::Drive);
    lock.armed = true;                                           // the next tick's queue check trips the latch
    sup.tick(hal.nowMs());
    CHECK(estop.tripped());
    CHECK(hal.command(Axis::Lifter).mode != Bridge::Drive);
}

// ---------------------------------------------------------------- 10: sequencer and random director

TEST(review10_an_unreadable_stored_sequence_is_skipped) {
    Rig g(gregConfig()); g.boot();
    MemSequenceStore store; XorShiftRandom rng{7}; VecSink out; Sequencer seq;
    struct L : Sequencer::Listener { void onLight(int) override {} void onRandom(const Command&) override {} void onRandomStop() override {} void onEstop() override {} } l;
    seq.begin(g.queue, g.pub, store, rng, out, l);
    store.write(5, ":PP50:PQ9");                                 // not storable today: flash damage, or a later grammar
    Command els[Sequencer::kMaxElements]; char bad[32];
    int n = Sequencer::parseChain(":PS5:PL3", els, Sequencer::kMaxElements, bad, sizeof bad);
    CHECK_EQ(n, 2);
    seq.submit(els, n, ReqSource::SerialUsb, false);
    for (int i = 0; i < 50; ++i) { seq.tick(g.hal.nowMs()); g.runMs(1); }
    CHECK(out.count("unreadable") == 1);
}

TEST(review10_random_mode_ends_itself_on_an_estop) {
    XorShiftRandom rng{3}; VecSink out; RandomDirector dir; dir.begin(rng, out);
    dir.startLevel(1, 0);
    Snapshot s; s.state = MotionState::Idle; s.lifterTravel = 450; s.lifterTicks = 450;
    RandomDirector::Limits lim; Command ch[Sequencer::kMaxElements];
    s.estop = true;
    int n = dir.next(100000, true, s, lim, ch, Sequencer::kMaxElements);
    CHECK_EQ(n, 0);
    CHECK(!dir.active());
}

// Huge typed numbers: ":PW4294968" wrapped to 0.7 s under the 60 s clamp.
TEST(review10_a_huge_wait_is_clamped_to_sixty_seconds) {
    Rig g(gregConfig()); g.boot();
    MemSequenceStore store; XorShiftRandom rng{7}; VecSink out; Sequencer seq;
    struct L : Sequencer::Listener { void onLight(int) override {} void onRandom(const Command&) override {} void onRandomStop() override {} void onEstop() override {} } l;
    seq.begin(g.queue, g.pub, store, rng, out, l);
    Command els[Sequencer::kMaxElements]; char bad[32];
    int n = Sequencer::parseChain(":PW4294968:PP50", els, Sequencer::kMaxElements, bad, sizeof bad);
    CHECK_EQ(n, 2);
    seq.submit(els, n, ReqSource::SerialUsb, false);
    for (int i = 0; i < 5000; ++i) { seq.tick(g.hal.nowMs()); g.runMs(1); }
    CHECK_EQ(jobStarts(g, JobKind::LifterSeek), 0);              // still waiting
    CHECK(out.count("WAIT clamped") == 1);
    MotionRequest r;
    Command big; big.verb = Verb::LiftTo; big.a = 50; big.hasA = true; big.b = 999999999; big.hasB = true;
    CHECK(buildRequest(big, 450, rng, r));
    CHECK(r.b >= 0 && r.b <= 1000);
    Command rel; rel.verb = Verb::RotRel; rel.a = 999999999; rel.hasA = true;
    CHECK(buildRequest(rel, 450, rng, r));
    CHECK(r.a <= 36000);
}

// ---------------------------------------------------------------- 11: lines on the bus

namespace {
struct NullCfg final : Dispatcher::ConfigHandler { int n = 0; void onConfig(const Command&, const char*, ReqSource) override { ++n; } };
}
// Hard rule 7: another board's ':' command on a shared Marcduino bus is a silent no-op here.
TEST(review11_marcduino_lines_for_other_boards_are_silent) {
    Rig g(gregConfig()); g.boot();
    MemSequenceStore store; XorShiftRandom rng{7}; VecSink out; Sequencer seq; Dispatcher disp; NullCfg cfg;
    struct L : Sequencer::Listener { void onLight(int) override {} void onRandom(const Command&) override {} void onRandomStop() override {} void onEstop() override {} } l;
    seq.begin(g.queue, g.pub, store, rng, out, l); disp.begin(seq, cfg, out);
    for (const char* line : {":SE00", ":OP01", ":CL00", ":SE10"}) CHECK(!disp.handleLine(line, ReqSource::SerialCmd, 1000));
    CHECK_EQ(out.count("INVALID"), 0);
    CHECK(!disp.handleLine(":PQ7", ReqSource::SerialCmd, 5000));  // a broken line of ours still says so
    CHECK_EQ(out.count("INVALID"), 1);
}

// A line longer than the assembler holds was cut and executed (":PP100" at the end became ":PP1").
TEST(review11_an_over_long_line_is_dropped_whole) {
    LineAssembler a;
    std::string s = ":PP50";
    while (s.size() < LineAssembler::kMax + 4) s += ":PL1";
    s += ":PP100\n";
    LineAssembler::Kind k = LineAssembler::Kind::None;
    for (char c : s) { LineAssembler::Kind r = a.feed(c); if (r != LineAssembler::Kind::None) k = r; }
    CHECK(k == LineAssembler::Kind::Dropped);
    std::string ok = "#PS100:";
    while (ok.size() < 261) ok += "P";                            // the longest #PS line #PCONFIG prints
    ok += "\n"; k = LineAssembler::Kind::None;
    for (char c : ok) { LineAssembler::Kind r = a.feed(c); if (r != LineAssembler::Kind::None) k = r; }
    CHECK(k == LineAssembler::Kind::Command);
}

// Bytes lost on the way in (a full FIFO, the UART's own overflow) spliced two lines into one valid
// command: "#PSAFESPIN878" + "#PDRIFT=0" became "#PSAFESPIN80". A gap now spoils its line.
TEST(review11_lost_serial_bytes_spoil_the_line_instead_of_splicing_it) {
    SerialFifo<12> fifo; LineAssembler a;
    std::vector<std::string> got;
    auto drain = [&] { uint8_t b; while (fifo.pop(b)) if (a.feed(static_cast<char>(b)) == LineAssembler::Kind::Command) got.push_back(a.line()); };
    for (const char* p = "#PSAFESPIN878\r\n#PDRIFT="; *p; ++p) fifo.push(static_cast<uint8_t>(*p));   // loop() late: 11 bytes lost
    drain();
    for (const char* p = "0\r\n#PSTATE\r\n"; *p; ++p) fifo.push(static_cast<uint8_t>(*p));
    drain();
    CHECK(fifo.dropped() > 0);
    for (auto& l : got) CHECK(l == "#PSAFESPIN878" || l == "#PDRIFT=0" || l == "#PSTATE");   // never "#PSAFESPIN870"
    CHECK(!got.empty() && got.back() == "#PSTATE");                                          // the next whole line is fine
}

// ---------------------------------------------------------------- 12: settings

// Flashing an older build over a newer one: the newer blob was "unreadable" and overwritten with
// defaults (INVERT 0, SAFESPIN from v3, LOCKED). The known part is now kept and the blob left alone.
TEST(review12_settings_from_a_newer_schema_keep_what_this_build_knows) {
    Settings todd{}; todd.inverted = 1; todd.safeSpinTicks = 878; todd.commissioned = 2; todd.ticksPerRev = 1185.0f; todd.tprConfident = 1;
    todd.schema = kSettingsSchema + 1;
    uint8_t blob[sizeof(Settings) + 16] = {};
    std::memcpy(blob, &todd, sizeof todd);
    Settings out;
    uint16_t from = migrateSettings(out, blob, sizeof blob);
    CHECK_EQ(from, kSettingsSchema + 1);
    CHECK_EQ(out.inverted, 1); CHECK_EQ(out.safeSpinTicks, 878); CHECK_EQ(out.commissioned, 2);
    Calibration cal{}; cal.travel = 1137; cal.valid = 1; cal.bottomRest = 37; cal.schema = kCalibrationSchema + 1;
    Calibration cout;
    CHECK_EQ(migrateCalibration(cout, reinterpret_cast<const uint8_t*>(&cal), sizeof cal), kCalibrationSchema + 1);
    CHECK_EQ(cout.travel, 1137); CHECK_EQ(cout.bottomRest, 37);
    uint8_t tiny[9] = {};
    CHECK_EQ(migrateSettings(out, tiny, sizeof tiny), 0);         // not even the header
}

// A 1-7 character #PAPPASS silently became the published default "Astromech" and was saved.
TEST(review12_a_short_access_point_password_changes_nothing) {
    Settings s{}; std::snprintf(s.apPass, sizeof s.apPass, "R2Secret99");
    const StringKey* k = findStringKey("APPASS", 6);
    CHECK(k != nullptr);
    CHECK(!acceptStringValue(*k, "short"));
    CHECK(!acceptStringValue(*k, ""));
    CHECK(acceptStringValue(*k, "LongEnough1"));
    const StringKey* ssid = findStringKey("SSID", 4);
    CHECK(acceptStringValue(*ssid, ""));                          // clearing the home network is allowed
}

// #PV3IMPORT (forced) replaced measured numbers with v3's: 1185.0 ticks/rev became 1191, the safe
// spin height 878 became 904, the calibrated minimum powers became 50 %.
TEST(review12_a_forced_v3_import_never_overwrites_a_measured_value) {
    Settings s{}; s.ticksPerRev = 1185.0f; s.tprConfident = 1; s.safeSpinTicks = 878; s.commissioned = 2; s.inverted = 1;
    V3Prefs v; v.present = true; v.rotenccount = 1191; v.minheight = 904; v.lftminpwr = 50; v.lftdist = 1350; v.motortype = 0;
    ImportReport rep;
    importV3(s, v, rep, true, true);                              // forced, on a calibrated board
    CHECK_NEAR(s.ticksPerRev, 1185.0, 0.01); CHECK_EQ(s.tprConfident, 1);
    CHECK_EQ(s.safeSpinTicks, 878);
    CHECK_EQ(s.minPowerUpPct, 0); CHECK_EQ(s.minPowerDownPct, 0);
    Settings fresh{}; ImportReport rep2;                          // a first boot still takes them
    importV3(fresh, v, rep2, false);
    CHECK_NEAR(fresh.ticksPerRev, 1191.0, 0.01); CHECK_EQ(fresh.safeSpinTicks, 904); CHECK_EQ(fresh.minPowerUpPct, 50);
}

// ---------------------------------------------------------------- 13: intake holds

// An interrupted page upload left the intake held for ever, and the next settings save released it
// in the middle of a running upload. Each holder now has its own bit.
TEST(review13_intake_holds_belong_to_their_owner) {
    Rig g(gregConfig()); g.hal.lifter.setPosition(0); g.boot();
    g.sup.setHoldIntake(MotionSupervisor::kHoldOta, true);
    g.sup.setHoldIntake(MotionSupervisor::kHoldPersist, true);
    g.sup.setHoldIntake(MotionSupervisor::kHoldPersist, false);   // a settings save finishes mid-upload
    g.seek(300, 650); g.runMs(500);
    CHECK_EQ(jobStarts(g, JobKind::LifterSeek), 0);
    g.sup.setHoldIntake(MotionSupervisor::kHoldOta, false);
    g.runMs(50);
    CHECK_EQ(jobStarts(g, JobKind::LifterSeek), 1);
}

// ---------------------------------------------------------------- 14: a slow loop() and the drivers' enable

// The drivers are switched on from loop(); a loop() pass held up by a long print (#PCONFIG on USB is
// ~60 ms) made the job fail with a sticky EXPANDER fault that only #PCLEARFAULT cleared.
TEST(review14_a_slow_enable_acknowledgement_is_not_an_expander_fault) {
    Rig g(gregConfig()); g.hal.lifter.setPosition(0); g.boot();
    g.hal.enableLatencyMs = 120;
    g.seek(300, 650); g.runUntilIdle(20000);
    CHECK(!(g.snap().faults & F_Expander));
    CHECK(g.snap().lastResult == JobResult::DoneTarget || g.snap().lastResult == JobResult::Done);
    g.hal.failEnable = true;                                       // a real expander fault is still one
    g.runMs(3000); g.seek(100, 650); g.runUntilIdle(20000);
    CHECK(g.snap().faults & F_Expander);
}

// ---------------------------------------------------------------- 15: #PCONFIG prints a long stored sequence whole

TEST(review15_a_long_line_prints_whole) {
    VecSink out;
    std::string body(254, 'P');
    out.printf("#PS%d:%s", 100, body.c_str());
    CHECK_EQ(out.lines.back().size(), 7u + 254u);
}

// "Set home here" with the head on the switch re-references the count in the same tick: that jump
// is not travel, and must not read as a switch stuck closed on the next move.
TEST(review03_set_home_here_on_the_switch_is_not_a_stuck_switch) {
    MotionConfig c = gregConfig(false); c.rotary.homeOffsetTicks = 300;          // the visual home sits 300 ticks from the switch
    Rig g(c); toddArc(g); g.hal.lifter.setPosition(450); g.setRotaryAngleFromCentre(0); g.boot();
    CHECK(g.hal.rotary.homeSwitch());
    g.sup.requestSetRotaryHome(); g.runMs(5);                                    // the count jumps 0 -> 300 with the switch closed
    g.req(JobKind::RotaryRelative, 90, 0, 0); g.runUntilIdle(30000);
    CHECK(!(g.snap().faults & F_HomeSwitchStuck));
}
