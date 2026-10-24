// Phase 5: the WCB mesh's pure side (MeshProtocol.h), the mesh settings (schema 5), the dedup ring,
// and the mesh as an unattended source through the real dispatcher, sequencer and supervisor.
#include "rig.h"
#include "../../src/core/MeshProtocol.h"
#include "../../src/core/Settings.h"
#include "../../src/core/Sequencer.h"
#include "../../src/core/Dispatcher.h"
#include "../../src/core/LineQueue.h"
#include <string>
#include <vector>
using namespace uppity;
using namespace uppity::rig;

namespace {

struct Sink final : TextSink {
    std::vector<std::string> lines;
    void line(const char* t) override { lines.push_back(t); }
    bool has(const char* n) const { for (auto& l : lines) if (l.find(n) != std::string::npos) return true; return false; }
};

struct Radio final : MeshReporter::Out {
    std::vector<std::string> bcast, ctrl;
    std::vector<bool> ensured;
    bool controllerUp = true;
    bool broadcast(const char* t) override { bcast.push_back(t); return true; }
    bool toController(const char* t, bool e) override { if (!controllerUp) return false; ctrl.push_back(t); ensured.push_back(e); return true; }
    int count(const std::vector<std::string>& v, const char* n) const { int k = 0; for (auto& l : v) if (l.find(n) != std::string::npos) ++k; return k; }
};

Command cmdOf(const char* line) { Command c; parseLine(line, c); return c; }

Snapshot snapAt(int pct, int deg, MotionState st = MotionState::Idle) {
    Snapshot s; s.state = st; s.lifterConf = LifterConfidence::Known; s.lifterPct = static_cast<int16_t>(pct);
    s.homeConf = HomeConfidence::Latched; s.ticksPerRev = 1185.f; s.rotaryDeg = static_cast<int16_t>(deg); s.commissioned = true;
    return s;
}

// A dispatcher + sequencer on the motion rig, as loop() wires them (the mesh is one more source).
struct MeshRig : Sequencer::Listener, Dispatcher::ConfigHandler {
    Rig g;
    MemSequenceStore store;
    XorShiftRandom rng{7};
    Sink out;
    Sequencer seq;
    Dispatcher disp;
    std::vector<Verb> configs;
    bool meshGate = true;
    explicit MeshRig(MotionConfig cfg) : g(cfg) { seq.begin(g.queue, g.pub, store, rng, out, *this); disp.begin(seq, *this, out); }
    void onLight(int) override {}
    void onRandom(const Command&) override {}
    void onRandomStop() override {}
    void onEstop() override { g.estop.trip(EstopLatch::Source::Sequence, g.hal.nowMs()); }
    // what Console::onConfig does first
    void onConfig(const Command& c, const char*, ReqSource src) override {
        if (meshGate && src == ReqSource::Mesh && !meshMayRunConfig(c.verb)) { out.line("MESH=refused"); return; }
        configs.push_back(c.verb);
    }
    bool line(const char* l, ReqSource src) { return disp.handleLine(l, src, g.hal.nowMs()); }
    void run(int ms) { for (int i = 0; i < ms; ++i) { seq.tick(g.hal.nowMs()); g.runMs(1); } }
};

} // namespace

// ---------------------------------------------------------------- inbound

TEST(mesh_classifies_its_verbs_and_stays_silent_for_everything_else) {
    CHECK(classifyMesh(":PP100") == MeshMsg::Line);
    CHECK(classifyMesh("  #PSTATE") == MeshMsg::Line);
    CHECK(classifyMesh(":SE00") == MeshMsg::Line);                 // the dispatcher drops other boards' ':' lines silently
    CHECK(classifyMesh("&ESTOP") == MeshMsg::Estop);
    CHECK(classifyMesh("&ESTOP\r\n") == MeshMsg::Estop);
    CHECK(classifyMesh("&ESTOP,web") == MeshMsg::Estop);
    CHECK(classifyMesh("&ESTOP,RESET") == MeshMsg::EstopReset);
    CHECK(classifyMesh("&ESTOP,RESETX") == MeshMsg::Foreign);
    CHECK(classifyMesh("&DISABLE") == MeshMsg::Disable);
    CHECK(classifyMesh("&STATUS") == MeshMsg::Status);
    CHECK(classifyMesh("&HELLO") == MeshMsg::Hello);
    CHECK(classifyMesh("?STOP") == MeshMsg::StopAll);
    CHECK(classifyMesh("&SABE,HB,0.41.0,123,SAFE_IDLE,-1,-1") == MeshMsg::Foreign);
    CHECK(classifyMesh("&RAD,POS,90,AUTO") == MeshMsg::Foreign);
    CHECK(classifyMesh("&PERI,HB,4.0.0,1,IDLE,100,0,1") == MeshMsg::Foreign);   // our own kind, from another periscope
    CHECK(classifyMesh("&ENABLE") == MeshMsg::Foreign);            // nothing on the mesh arms anything
    CHECK(classifyMesh("&estop") == MeshMsg::Foreign);             // case-sensitive, as Sabé
    CHECK(classifyMesh(";W3;S4:PP100") == MeshMsg::Foreign);
    CHECK(classifyMesh("") == MeshMsg::Foreign);
    CHECK(classifyMesh(nullptr) == MeshMsg::Foreign);
}

TEST(mesh_estop_patterns_are_the_serial_ports_own) {
    CHECK(meshWantsEstop("&ESTOP"));
    CHECK(meshWantsEstop("&ESTOP,mesh"));
    CHECK(meshWantsEstop("?STOP"));
    CHECK(meshWantsEstop(":PX"));
    CHECK(meshWantsEstop("&SABE,ESTOP,console"));                  // Sabé's own latch, broadcast
    CHECK(!meshWantsEstop("&ESTOP,RESET"));
    CHECK(!meshWantsEstop("&PERI,ACK,ESTOP"));                     // our own receipt, echoed back by a WCB
    CHECK(!meshWantsEstop("&PERI,ACK,ESTOP.RESET"));
    CHECK(!meshWantsEstop(":PP0"));
    CHECK(!meshWantsEstop("&DISABLE"));
}

TEST(mesh_may_move_and_ask_but_not_configure) {
    CHECK(meshMayRunConfig(cmdOf("#PSTATE").verb));
    CHECK(meshMayRunConfig(cmdOf("#PSTATUS").verb));
    CHECK(meshMayRunConfig(cmdOf("#PCAL").verb));
    CHECK(meshMayRunConfig(cmdOf("#PRESUME").verb));
    for (const char* l : { "#PCONFIG", "#PL", "#PV3",                    // they read flash (hard rule 5): not from an unattended source
                           "#PUNLOCK", "#PCOMMISSIONBYPASS", "#PCOMMISSIONRUN", "#POVERRIDE", "#PSETHOME", "#PFACTORY", "#PRESTART",
                           "#PSAFESPIN0", "#PMESH0", "#PMESHPASS=x", "#PS1::PP100", "#PSC", "#PNUDGE-20", "#PJOGL-", "#PWIFIRESET", "#PV3IMPORT" })
        if (meshMayRunConfig(cmdOf(l).verb)) { std::printf("  allowed from the mesh: %s\n", l); CHECK(false); }
}

// ---------------------------------------------------------------- outbound

TEST(mesh_peri_lines_fit_one_packet_and_say_unknown_as_minus_one) {
    char b[kMeshPacketMax + 1];
    PeriStatus p = periStatus(snapAt(100, 0));
    CHECK(formatMeshHb(b, sizeof b, "4.0.0-dev", 86400 * 30, p) > 0);
    CHECK(std::string(b) == "&PERI,HB,4.0.0-dev,2592000,IDLE,100,0,1");
    Snapshot u; u.state = MotionState::Locked;                     // power-up: nothing known
    p = periStatus(u);
    formatMeshHb(b, sizeof b, "4.0.0-dev", 5, p); CHECK(std::string(b) == "&PERI,HB,4.0.0-dev,5,LOCKED,-1,-1,0");
    Snapshot e = snapAt(50, 90); e.estop = true; e.state = MotionState::Stopped;
    p = periStatus(e);
    formatMeshPos(b, sizeof b, p); CHECK(std::string(b) == "&PERI,POS,50,90");
    formatMeshState(b, sizeof b, p); CHECK(std::string(b) == "&PERI,STATE,ESTOP");
    formatMeshFault(b, sizeof b, F_LifterStall); CHECK(std::string(b) == "&PERI,FAULT,LIFTER_STALL");
    formatMeshHello(b, sizeof b, "4.0.0-dev"); CHECK(std::string(b) == "&PERI,HELLO,4.0.0-dev,1");
    formatMeshAck(b, sizeof b, MeshMsg::EstopReset); CHECK(std::string(b) == "&PERI,ACK,ESTOP.RESET");
    CHECK(formatMeshAck(b, sizeof b, MeshMsg::Foreign) < 0);
    char longFw[200]; std::memset(longFw, 'x', sizeof longFw - 1); longFw[sizeof longFw - 1] = '\0';
    CHECK(formatMeshHb(b, sizeof b, longFw, 1, p) < 0);           // never a fragment: a broadcast cannot carry one
}

TEST(mesh_status_height_angle_and_homed) {
    PeriStatus p = periStatus(snapAt(100, 0));     CHECK_EQ(p.heightPct, 100); CHECK_EQ(p.rotDeg, 0); CHECK_EQ(p.homed, 1);
    p = periStatus(snapAt(80, 355));               CHECK_EQ(p.homed, 1);                    // within 10 degrees of home
    p = periStatus(snapAt(80, 90));                CHECK_EQ(p.homed, 0); CHECK_EQ(p.rotDeg, 90);
    Snapshot s = snapAt(80, 90); s.lifterConf = LifterConfidence::Stale;   p = periStatus(s); CHECK_EQ(p.heightPct, -1);   // a stalled count is not a height
    s = snapAt(80, 90); s.homeConf = HomeConfidence::Unknown;              p = periStatus(s); CHECK_EQ(p.rotDeg, -1); CHECK_EQ(p.homed, 0);
    s = snapAt(80, 90); s.rotaryDisabled = true;                           p = periStatus(s); CHECK_EQ(p.rotDeg, -1); CHECK_EQ(p.homed, 1);
}

TEST(mesh_reporter_heartbeat_position_state_and_faults) {
    MeshReporter r; Radio radio;
    r.start(0, 10);
    Snapshot s = snapAt(0, 0);
    Ms t = 0;
    auto run = [&](Ms until, const Snapshot& snap) { for (; t <= until; t += 10) r.tick(snap, t, t / 1000, "4.0.0", radio); };
    run(2900, s); CHECK_EQ(radio.count(radio.bcast, "&PERI,HB"), 0);       // the library's own boot announces go first
    run(3100, s); CHECK_EQ(radio.count(radio.bcast, "&PERI,HB"), 1);
    run(12900, s); CHECK_EQ(radio.count(radio.bcast, "&PERI,HB"), 1);
    run(13100, s); CHECK_EQ(radio.count(radio.bcast, "&PERI,HB"), 2);      // every 10 s
    CHECK_EQ(radio.count(radio.ctrl, "&PERI,POS"), 1);                      // the first position, once
    // a 3 s rise: positions to Sabé at most once a second, unacknowledged
    for (int i = 0; i < 300; ++i) { Snapshot m = snapAt(i / 3, 0, MotionState::Moving); r.tick(m, t, t / 1000, "4.0.0", radio); t += 10; }
    int pos = radio.count(radio.ctrl, "&PERI,POS");
    CHECK(pos >= 3 && pos <= 5);
    for (bool e : radio.ensured) if (e) CHECK(false);
    CHECK_EQ(radio.count(radio.bcast, "&PERI,STATE,MOVING"), 1);           // a transition, once
    run(t + 2000, snapAt(100, 0)); CHECK_EQ(radio.count(radio.bcast, "&PERI,STATE,IDLE"), 1);
    size_t before = radio.ctrl.size(); run(t + 5000, snapAt(100, 0)); CHECK_EQ(radio.ctrl.size(), before);   // nothing changes: nothing sent
    // a fault: ensured to Sabé; with Sabé away, broadcast instead
    Snapshot f = snapAt(100, 0); f.faults = F_LifterStall;
    r.tick(f, t, t / 1000, "4.0.0", radio); t += 10;
    CHECK_EQ(radio.count(radio.ctrl, "&PERI,FAULT,LIFTER_STALL"), 1); CHECK(radio.ensured.back());
    r.tick(f, t, t / 1000, "4.0.0", radio); t += 10;
    CHECK_EQ(radio.count(radio.ctrl, "&PERI,FAULT"), 1);                    // once per new fault
    radio.controllerUp = false;
    f.faults |= F_HomeSwitchStuck; r.tick(f, t, t / 1000, "4.0.0", radio); t += 10;
    CHECK_EQ(radio.count(radio.bcast, "&PERI,FAULT,HOME_SWITCH_STUCK"), 1);
    for (auto& l : radio.bcast) CHECK(l.size() <= kMeshPacketMax);
}

TEST(mesh_status_request_sends_everything_now) {
    MeshReporter r; Radio radio; r.start(0, 300);
    r.burst(snapAt(42, 180), 500, 0, "4.0.0", radio);
    CHECK_EQ(radio.count(radio.bcast, "&PERI,HB,4.0.0,0,IDLE,42,180,0"), 1);
    CHECK_EQ(radio.count(radio.bcast, "&PERI,STATE,IDLE"), 1);
    CHECK_EQ(radio.count(radio.ctrl, "&PERI,POS,42,180"), 1);
}

// ---------------------------------------------------------------- settings (schema 5)

TEST(mesh_settings_append_off_by_default_and_migrate_from_schema_4) {
    static_assert(sizeof(Settings) % 4 == 0, "layout");
    static_assert(offsetof(Settings, meshHbS) == 224, "appended after the schema 3/4 block");
    Settings d; CHECK_EQ(d.meshEnabled, 0); CHECK_EQ(d.meshHbS, 10); CHECK_EQ(d.meshChannel, 1); CHECK_EQ(d.meshCrc, 1); CHECK(meshMissing(d) != nullptr);
    Settings old; old.schema = 4; old.travelTicks = 1150; old.safeSpinTicks = 878; old.commissioned = 2; old.commissionStep = 8;
    std::snprintf(old.staSsid, sizeof old.staSsid, "WordNet");
    uint8_t blob[224]; std::memcpy(blob, &old, sizeof blob);           // what a schema-4 board has in flash
    Settings m; CHECK_EQ(migrateSettings(m, blob, sizeof blob), 4);
    CHECK_EQ(m.schema, kSettingsSchema); CHECK_EQ(m.safeSpinTicks, 878); CHECK_EQ(m.commissioned, 2); CHECK(std::string(m.staSsid) == "WordNet");
    CHECK_EQ(m.meshEnabled, 0); CHECK_EQ(m.meshHbS, 10); CHECK_EQ(m.meshChannel, 1); CHECK_EQ(m.meshCrc, 1); CHECK_EQ(m.meshPass[0], '\0');
    Settings w; w.meshHbS = 2; w.meshId = 20; w.meshQty = 40; w.meshChannel = 14; w.meshEnabled = 7; sanitize(w);
    CHECK_EQ(w.meshHbS, 10); CHECK_EQ(w.meshId, 0); CHECK_EQ(w.meshQty, 0); CHECK_EQ(w.meshChannel, 1); CHECK_EQ(w.meshEnabled, 1);
}

TEST(mesh_mac_octets_read_as_the_wcbs_show_them) {
    uint8_t a = 0, b = 0;
    CHECK(parseMeshMac("3C:4E", a, b)); CHECK_EQ(a, 0x3C); CHECK_EQ(b, 0x4E);
    CHECK(parseMeshMac("3c-4e", a, b)); CHECK_EQ(b, 0x4E);
    CHECK(parseMeshMac("0x3C:0x4E", a, b)); CHECK_EQ(a, 0x3C);
    CHECK(parseMeshMac(" 3C 4E ", a, b));
    CHECK(parseMeshMac("3C4E", a, b)); CHECK_EQ(a, 0x3C); CHECK_EQ(b, 0x4E);
    CHECK(!parseMeshMac("3C", a, b)); CHECK(!parseMeshMac("3C:4E:01", a, b)); CHECK(!parseMeshMac("ZZ:4E", a, b)); CHECK(!parseMeshMac("", a, b));
    CHECK(!parseMeshMac("3C:4EZ", a, b));
}

TEST(mesh_needs_every_value_before_it_starts) {
    Settings s; s.meshEnabled = 1;
    CHECK(std::string(meshMissing(s)).find("MESHMAC") != std::string::npos);
    std::snprintf(s.meshMac, sizeof s.meshMac, "3C:4E");
    CHECK(std::string(meshMissing(s)).find("MESHPASS") != std::string::npos);
    std::snprintf(s.meshPass, sizeof s.meshPass, "not-a-real-one");
    CHECK(std::string(meshMissing(s)).find("MESHQTY") != std::string::npos);
    s.meshQty = 3;
    CHECK(std::string(meshMissing(s)).find("MESHID") != std::string::npos);
    s.meshId = 5;
    CHECK(meshMissing(s) == nullptr);
}

TEST(mesh_keys_parse_and_the_password_is_never_printed) {
    Command c = cmdOf("#PMESHMAC=3C:4E");   CHECK(c.verb == Verb::Set); CHECK(std::string(c.key) == "MESHMAC"); CHECK(c.text && std::string(c.text) == "3C:4E");
    c = cmdOf("#PMESHPASS=two words!");     CHECK(c.verb == Verb::Set); CHECK(std::string(c.key) == "MESHPASS"); CHECK(c.text && std::string(c.text) == "two words!");
    c = cmdOf("#PMESHID5");                 CHECK(std::string(c.key) == "MESHID"); CHECK(c.hasA); CHECK_EQ(c.a, 5);
    c = cmdOf("#PMESH1");                   CHECK(std::string(c.key) == "MESH"); CHECK_EQ(c.a, 1);
    c = cmdOf("#PMESHHB=30");               CHECK(std::string(c.key) == "MESHHB"); CHECK_EQ(c.a, 30);
    const StringKey* pass = findStringKey("MESHPASS", 8); CHECK(pass && pass->secret);
    const StringKey* mac = findStringKey("MESHMAC", 7);   CHECK(mac && !mac->secret);
    CHECK(acceptStringValue(*mac, "3C:4E")); CHECK(!acceptStringValue(*mac, "hello")); CHECK(acceptStringValue(*mac, ""));
    std::string forty(40, 'p'); CHECK(!acceptStringValue(*pass, forty.c_str())); CHECK(acceptStringValue(*pass, forty.substr(0, 39).c_str()));
    char v[80]; canonicalStringValue(*mac, "0x3c:0x4e", v, sizeof v); CHECK(std::string(v) == "3C:4E");   // fits the 5-character field
    Settings st; setStringField(st, *mac, v); CHECK(std::string(st.meshMac) == "3C:4E");
    canonicalStringValue(*pass, "Pa:ss", v, sizeof v); CHECK(std::string(v) == "Pa:ss");
    for (int i = 0; i < kSettingKeyCount; ++i) {                           // no key name has a digit: "#PKEY123" splits at the first one
        for (const char* p = kSettingKeys[i].name; *p; ++p) CHECK(*p >= 'A' && *p <= 'Z');
        CHECK(std::strlen(kSettingKeys[i].name) <= 12);                     // the page's settings POST takes 12
    }
    for (int i = 0; i < kStringKeyCount; ++i) for (const char* p = kStringKeys[i].name; *p; ++p) CHECK(*p >= 'A' && *p <= 'Z');
}

// Code review D8: a ticks/rev nobody measured replays as 0 (measure again), not as measured.
TEST(config_dump_writes_an_unmeasured_ticks_per_rev_as_zero) {
    const SettingKey* k = findSettingKey("ROTARYTPR", 9); CHECK(k != nullptr);
    if (!k) return;
    char buf[64];
    Settings v3; v3.ticksPerRev = 1191.f; v3.tprConfident = 0;              // v3's figure, imported
    formatConfigLine(v3, *k, buf, sizeof buf); CHECK(std::string(buf) == "#PROTARYTPR=0");
    Settings todd; todd.ticksPerRev = 1185.f; todd.tprConfident = 1;        // measured
    formatConfigLine(todd, *k, buf, sizeof buf); CHECK(std::string(buf) == "#PROTARYTPR=11850");
    Settings none; formatConfigLine(none, *k, buf, sizeof buf); CHECK(std::string(buf) == "#PROTARYTPR=0");
}

// ---------------------------------------------------------------- dedup: the wire and the mesh

TEST(dedup_catches_a_mesh_copy_with_other_lines_in_between) {
    LineDedup d;
    CHECK(d.accept(":PP100", ReqSource::SerialCmd, 1000, 750));
    CHECK(d.accept(":PA90", ReqSource::SerialCmd, 1050, 750));
    CHECK(d.accept(":PL3", ReqSource::SerialUsb, 1080, 750));
    CHECK(!d.accept(":PP100", ReqSource::Mesh, 1200, 750));             // before: only the last line was remembered
    CHECK(!d.accept(":PA90", ReqSource::Mesh, 1250, 750));
    CHECK(d.accept(":PP100", ReqSource::Mesh, 1900, 750));               // outside the window: runs
}

// ---------------------------------------------------------------- the mesh as a source

TEST(mesh_lines_drive_through_the_dispatcher_and_foreign_lines_are_silent) {
    MeshRig r(gregConfig(true)); r.g.hal.lifter.setPosition(0); r.g.boot();
    CHECK(r.line(":PP100", ReqSource::Mesh));
    r.run(20000);
    CHECK(r.g.hal.lifter.topLimit());
    CHECK(!r.line(":SE00", ReqSource::Mesh));                            // the dome panels' line: nothing, no INVALID
    CHECK(!r.line("#DPCONFIG", ReqSource::Mesh));                        // Roam-A-Dome's: nothing
    CHECK(!r.out.has("INVALID"));
    CHECK(r.line("#PSTATE", ReqSource::Mesh)); CHECK_EQ(r.configs.size(), 1u);
    r.line("#PCONFIG", ReqSource::Mesh); CHECK_EQ(r.configs.size(), 1u);
    r.line("#PUNLOCK", ReqSource::Mesh); r.line("#PSAFESPIN0", ReqSource::Mesh); r.line("#POVERRIDE", ReqSource::Mesh);
    CHECK_EQ(r.configs.size(), 1u); CHECK(r.out.has("MESH=refused"));
}

// The review's finding 5 for the mesh: during a rescue the override is live, but a ":PP0" from the
// mesh is unattended and never gets it. The turned head stops at the safe-spin height (watched).
TEST(mesh_never_gets_the_override_for_a_turned_head) {
    MeshRig r(gregConfig(false));
    r.g.hal.lifter.setPosition(450); r.g.setRotaryAngleFromCentre(200); r.g.boot();
    r.g.req(JobKind::RotaryHome); r.g.runUntilIdle(60000);
    r.g.req(JobKind::RotaryAbsolute, 90, 0, 0); r.g.runUntilIdle(30000);
    r.g.queue.setOverride(r.g.hal.nowMs() + CommandQueue::kOverrideMs);
    r.g.watchSafe = 225;
    CHECK(r.line(":PP0", ReqSource::Mesh));
    r.run(15000);
    CHECK_EQ(r.g.turnedBelowSafe, 0);
    CHECK(r.g.hal.lifter.position() >= 225.0);
    CHECK(!r.g.hal.lifter.bottomLimit());
}

// A mesh E-STOP lands on the same latch as every other source, and the supervisor brakes the mast to
// a stop with the drivers on (Phase 4 lesson: the back-driving screw falls on if they go off).
TEST(mesh_estop_lands_on_the_latch_and_brakes_a_descent) {
    MeshRig r(backdrivingConfig()); r.g.hal.lifter.setParams(backdrivingLifter());
    r.g.hal.lifter.setPosition(1150); r.g.setRotaryAngleFromCentre(0); r.g.boot();
    CHECK(r.line(":PP0", ReqSource::Mesh));
    for (int i = 0; i < 5000 && r.g.hal.lifter.position() > 600; ++i) r.run(1);
    double at = r.g.hal.lifter.position(); Ms t = r.g.hal.nowMs();
    if (meshWantsEstop("&ESTOP")) r.g.estop.trip(EstopLatch::Source::Mesh, t);   // what the radio task does
    r.run(1500);
    CHECK(r.g.estop.tripped()); CHECK(r.g.estop.source() == EstopLatch::Source::Mesh);
    CHECK(!r.g.hal.lifter.moving()); CHECK(at - r.g.hal.lifter.position() < 60);
    CHECK(!r.g.anyDriveAfter(t));
    CHECK(!r.line(":PP100", ReqSource::Mesh) || (r.run(500), !r.g.anyDriveAfter(t)));   // refused while latched
    r.g.estop.release(r.g.hal.nowMs());                                  // &ESTOP,RESET: released, nothing resumes by itself
    r.run(500); CHECK(!r.g.anyDriveAfter(t));
}

// ---------------------------------------------------------------- review of the mesh (2026-10-05)

// A long message is dropped whole, never cut short and run: WCB_Client reassembles unicasts of up to
// ~2.8 KB, and a 208-character chain ending ":PP100" used to come out as ":PP10".
TEST(mesh_long_or_multi_line_messages_are_dropped_whole) {
    char out[208];
    CHECK(meshCleanLine(":PP100\r\n", out, sizeof out)); CHECK(std::string(out) == ":PP100");
    CHECK(meshCleanLine("#PSTATE", out, sizeof out));
    std::string chain; while (chain.size() < 202) chain += ":PW1"; chain += ":PP100";
    CHECK(chain.size() >= 208);
    CHECK(!meshCleanLine(chain.c_str(), out, sizeof out));
    CHECK(!meshCleanLine(":PP100\r\n#PUNLOCK", out, sizeof out));    // a second line would reach the bus when the log is echoed there
    CHECK(!meshCleanLine(":PP1\x00" "00", out, sizeof out) || std::string(out) == ":PP1");
    CHECK(!meshCleanLine("\r\n", out, sizeof out));
    CHECK(!meshCleanLine(":PP10\x1b", out, sizeof out));
    CHECK(meshWantsEstop("&ESTOP\r\n:PP100"));                           // the latch still reads the raw message first
    NoLock lock; LineQueue<4, 16> q(lock);                                // and the queue itself never cuts a line
    CHECK(q.push("short")); CHECK(!q.push("a line of sixteen")); CHECK_EQ(q.dropped(), 1u);
    char b[32]; CHECK(q.pop(b, sizeof b)); CHECK(std::string(b) == "short"); CHECK(!q.pop(b, sizeof b));
}

TEST(mesh_channel_is_the_librarys_1_to_11) {
    const SettingKey* k = findSettingKey("MESHCHAN", 8); CHECK(k != nullptr);
    if (!k) return;
    Settings s; CHECK(k->set(s, 11)); CHECK(!k->set(s, 12)); CHECK(!k->set(s, 13)); CHECK_EQ(s.meshChannel, 11);
    s.meshChannel = 13; sanitize(s); CHECK_EQ(s.meshChannel, 1);
}

// ---------------------------------------------------------------- bench 2026-10-24 (WCBs on 7.0.1)

// The &STATUS answer (heartbeat, state, acknowledgement) left in one millisecond and WCB1 printed two of
// the three: broadcasts are not acknowledged on the air. The board's own broadcasts now leave in order,
// kMeshTxGapMs apart; a lone line still goes at once.
TEST(mesh_broadcasts_leave_in_order_and_spaced) {
    MeshTxPacer p; std::vector<std::pair<Ms, std::string>> sent;
    auto send = [&](Ms now) { p.service(now, [&](const char* t) { sent.emplace_back(now, t); }); };
    MeshReporter r; Radio burst; r.start(0, 300);
    r.burst(snapAt(100, 0), 500, 0, "4.0.0", burst);
    for (auto& l : burst.bcast) CHECK(p.push(l.c_str()));
    char ack[kMeshPacketMax + 1]; CHECK(formatMeshAck(ack, sizeof ack, MeshMsg::Status) > 0); CHECK(p.push(ack));
    for (Ms t = 500; t < 700; ++t) send(t);
    CHECK_EQ(sent.size(), static_cast<size_t>(3));
    if (sent.size() == 3) {
        CHECK(sent[0].second.find("&PERI,HB,") == 0);
        CHECK(sent[1].second == "&PERI,STATE,IDLE");
        CHECK(sent[2].second == "&PERI,ACK,STATUS");
        CHECK_EQ(sent[0].first, 500u);
        CHECK(sent[1].first - sent[0].first >= kMeshTxGapMs);
        CHECK(sent[2].first - sent[1].first >= kMeshTxGapMs);
    }
    sent.clear();                                                            // later, a lone heartbeat is not held back
    CHECK(p.push("&PERI,HB,4.0.0,10,IDLE,100,0,1")); send(10500);
    CHECK_EQ(sent.size(), static_cast<size_t>(1)); CHECK_EQ(p.pending(), 0);
    for (int i = 0; i < MeshTxPacer::kSlots; ++i) CHECK(p.push("x"));      // a full queue drops the newest, never cuts one
    CHECK(!p.push("y")); CHECK_EQ(p.dropped(), 1u); CHECK_EQ(p.pending(), MeshTxPacer::kSlots);
    std::string longest(kMeshPacketMax, 'a');
    MeshTxPacer q; CHECK(q.push(longest.c_str()));
    q.service(0, [&](const char* t) { CHECK_EQ(std::strlen(t), kMeshPacketMax); });
}
