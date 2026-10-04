#include "uppity_test.h"
#include "../../src/core/LineAssembler.h"
#include "../../src/core/Parser.h"
#include "../../src/core/EventFormat.h"
#include "../../src/core/Sequencer.h"
#include <string>
using namespace uppity;

static int trips(const char* s) { EstopScanner sc; int n = 0; for (const char* p = s; *p; ++p) if (sc.feed(*p)) ++n; return n; }

TEST(estop_scanner_triggers_ahead_of_any_buffer) {
    CHECK_EQ(trips(":PX\r"), 1);
    CHECK_EQ(trips(":PX\n"), 1);
    CHECK_EQ(trips(":PP100:PX\r"), 1);                       // mid-chain
    CHECK_EQ(trips("garbage:PX\r"), 1);                      // mid-line, no buffer needed
    CHECK_EQ(trips(":PXR\r"), 0);                            // not a trip
    CHECK_EQ(trips(":PX100\r"), 0);
    CHECK_EQ(trips("?STOP\r"), 1);
    CHECK_EQ(trips("&ESTOP\r"), 1);
    CHECK_EQ(trips("&ESTOP,RESET\r"), 0);                    // the release must never trip
    CHECK_EQ(trips("&SABE,ESTOP,console\r"), 1);             // Sabe's latch notice, at the comma
    CHECK_EQ(trips("&SABE,HB,0.45.0,120,OPERATIONAL,41,46\r"), 0);
    CHECK_EQ(trips(":PX\r:PX\r"), 2);
}

TEST(line_assembler_accepts_ours_and_drops_foreign_lines_whole) {
    LineAssembler la;
    std::string hb = "&SABE,HB,0.45.0,120,OPERATIONAL,41,46\r";
    LineAssembler::Kind k = LineAssembler::Kind::None;
    for (char c : hb) k = la.feed(c);
    CHECK(k == LineAssembler::Kind::Ecosystem);              // & lines are ecosystem verbs (Sabe's HB is ignored by the console)
    std::string junk = "READ: X [88]\r\n";
    k = LineAssembler::Kind::None; int drops = 0;
    for (char c : junk) { k = la.feed(c); if (k == LineAssembler::Kind::Dropped) ++drops; }
    CHECK_EQ(drops, 1);
    std::string cmd = ":PP100,50\r\n";
    LineAssembler::Kind got = LineAssembler::Kind::None;
    for (char c : cmd) { k = la.feed(c); if (k != LineAssembler::Kind::None) got = k; }
    CHECK(got == LineAssembler::Kind::Command);
    CHECK(std::string(la.line()) == ":PP100,50");
    // a foreign line mid-way never contaminates the next command
    std::string mixed = "&RAD,HB,2.0.0,10,IDLE\r:PA90\r";
    std::string last; for (char c : mixed) { k = la.feed(c); if (k == LineAssembler::Kind::Command) last = la.line(); }
    CHECK(last == ":PA90");
}

TEST(parser_motion_verbs) {
    Command c;
    CHECK(parseLine(":PP100,50", c)); CHECK(c.verb == Verb::LiftTo); CHECK_EQ(c.a, 100); CHECK(c.hasB); CHECK_EQ(c.b, 50);
    CHECK(parseLine(":PP0", c)); CHECK(c.verb == Verb::LiftTo); CHECK_EQ(c.a, 0); CHECK(!c.hasB);
    CHECK(parseLine(":PPR", c)); CHECK(c.verb == Verb::LiftRandom);
    CHECK(parseLine(":PH", c)); CHECK(c.verb == Verb::SafeLower); CHECK(!c.hasA);
    CHECK(parseLine(":PH40", c)); CHECK(c.verb == Verb::SafeLower); CHECK_EQ(c.a, 40);
    CHECK(parseLine(":PA90,40,100", c)); CHECK(c.verb == Verb::RotAbs); CHECK_EQ(c.a, 90); CHECK_EQ(c.b, 40); CHECK_EQ(c.c, 100);
    CHECK(parseLine(":PAR", c)); CHECK(c.verb == Verb::RotAbsRandom);
    CHECK(parseLine(":PD-720,60", c)); CHECK(c.verb == Verb::RotRel); CHECK_EQ(c.a, -720); CHECK_EQ(c.b, 60);
    CHECK(parseLine(":PA45,R", c)); CHECK(c.hasB); CHECK_EQ(c.b, -2);
    CHECK(parseLine(":PR-30", c)); CHECK(c.verb == Verb::Spin); CHECK_EQ(c.a, -30);
    CHECK(parseLine(":PR0", c)); CHECK(c.verb == Verb::Spin); CHECK_EQ(c.a, 0);
    CHECK(parseLine(":PRR60", c)); CHECK(c.verb == Verb::SpinRandom); CHECK_EQ(c.a, 60);
    CHECK(parseLine(":PX", c)); CHECK(c.verb == Verb::Estop);
    CHECK(parseLine(":P3P50", c)); CHECK(c.verb == Verb::LiftTo); CHECK_EQ(c.id, 3); CHECK_EQ(c.a, 50);
    CHECK(parseLine(":PL7", c)); CHECK(c.verb == Verb::Light); CHECK_EQ(c.a, 7);
    CHECK(parseLine(":PW5", c)); CHECK(c.verb == Verb::Wait); CHECK_EQ(c.a, 5);
    CHECK(parseLine(":PMA", c)); CHECK(c.verb == Verb::RandomLevel); CHECK_EQ(c.letter, 'A');
    CHECK(parseLine(":PQ", c)); CHECK(c.verb == Verb::Invalid);
    CHECK(parseLine(":PPabc", c)); CHECK(c.verb == Verb::Invalid);
}

TEST(parser_config_verbs_and_foreign_lines) {
    Command c;
    CHECK(parseLine("#PRESUME", c)); CHECK(c.verb == Verb::Resume);
    CHECK(parseLine("#PSTATE", c)); CHECK(c.verb == Verb::State);
    CHECK(parseLine("#PROTARYTEST5", c)); CHECK(c.verb == Verb::RotaryTest); CHECK_EQ(c.a, 5);
    CHECK(parseLine("#PROTARYTEST", c)); CHECK(c.verb == Verb::RotaryTest); CHECK(!c.hasA);
    CHECK(parseLine("#PSC", c)); CHECK(c.verb == Verb::Calibrate);
    CHECK(parseLine("#PMOTOR1", c)); CHECK(c.verb == Verb::Set); CHECK(std::string(c.key) == "MOTOR"); CHECK_EQ(c.a, 1);
    CHECK(parseLine("#PINVERT1", c)); CHECK(c.verb == Verb::Set); CHECK(std::string(c.key) == "INVERT"); CHECK(c.hasA); CHECK_EQ(c.a, 1);
    CHECK(parseLine("#PINVERT", c)); CHECK(c.verb == Verb::Set); CHECK(std::string(c.key) == "INVERT"); CHECK(!c.hasA);
    CHECK(parseLine("#PINVERT=1", c)); CHECK(c.verb == Verb::Set); CHECK(c.hasA); CHECK_EQ(c.a, 1);
    CHECK(parseLine("#PTRAVEL1150", c)); CHECK(c.verb == Verb::Set); CHECK(std::string(c.key) == "TRAVEL"); CHECK_EQ(c.a, 1150);
    CHECK(parseLine("#PSAFESPIN=904", c)); CHECK(c.verb == Verb::Set); CHECK(std::string(c.key) == "SAFESPIN"); CHECK_EQ(c.a, 904);
    CHECK(parseLine("#PV3", c)); CHECK(c.verb == Verb::V3Dump);
    CHECK(parseLine("#PV3IMPORT", c)); CHECK(c.verb == Verb::V3Import);
    CHECK(parseLine("#PHOMEOFFSET-18", c)); CHECK(c.verb == Verb::Set); CHECK(std::string(c.key) == "HOMEOFFSET"); CHECK_EQ(c.a, -18);
    CHECK(parseLine("#PROTARYTPR11855", c)); CHECK(c.verb == Verb::Set); CHECK(std::string(c.key) == "ROTARYTPR"); CHECK_EQ(c.a, 11855);
    CHECK(parseLine("#PROTARYTEST3", c)); CHECK(c.verb == Verb::RotaryTest); CHECK_EQ(c.a, 3);
    CHECK(parseLine("#PWIGGLE2", c)); CHECK(c.verb == Verb::Set); CHECK(std::string(c.key) == "WIGGLE"); CHECK_EQ(c.a, 2);
    CHECK(parseLine("#PCLEARFAULT", c)); CHECK(c.verb == Verb::ClearFault);
    CHECK(parseLine("#PBOGUS", c)); CHECK(c.verb == Verb::Set); CHECK(std::string(c.key) == "BOGUS");   // the console rejects unknown keys
    CHECK(parseLine("#PL", c)); CHECK(c.verb == Verb::ListSeq);
    CHECK(parseLine("#PLOCK", c)); CHECK(c.verb == Verb::Lock);
    CHECK(parseLine("#PD7", c)); CHECK(c.verb == Verb::DelSeq); CHECK_EQ(c.a, 7);
    CHECK(parseLine("#PDEBUG1", c)); CHECK(c.verb == Verb::Set); CHECK(std::string(c.key) == "DEBUG");
    CHECK(parseLine("#PR", c)); CHECK(c.verb == Verb::RotaryToggle);
    CHECK(parseLine("#PRESTART", c)); CHECK(c.verb == Verb::Restart);
    CHECK(parseLine("#PNCL", c)); CHECK(c.verb == Verb::LimitPolarity); CHECK_EQ(c.letter, 'C'); CHECK_EQ(c.a, 'L');
    CHECK(parseLine("#PNOR", c)); CHECK(c.verb == Verb::LimitPolarity); CHECK_EQ(c.letter, 'O'); CHECK_EQ(c.a, 'R');
    CHECK(parseLine("#PS3:P100:L5:R30", c)); CHECK(c.verb == Verb::StoreSeq); CHECK_EQ(c.a, 3); CHECK(std::string(c.text) == "P100:L5:R30");
    CHECK(parseLine("#PSEQNAME3=Search", c)); CHECK(c.verb == Verb::SeqName); CHECK_EQ(c.a, 3); CHECK(std::string(c.text) == "Search");
    CHECK(parseLine("#PSC", c)); CHECK(c.verb == Verb::Calibrate);
    CHECK(parseLine("#PSTATUS", c)); CHECK(c.verb == Verb::Status);
    CHECK(parseLine("#PZERO", c)); CHECK(c.verb == Verb::Zero);
    CHECK(parseLine("#PFACTORY", c)); CHECK(c.verb == Verb::Factory);
    CHECK(parseLine("#PID5", c)); CHECK(c.verb == Verb::Set); CHECK(std::string(c.key) == "ID"); CHECK_EQ(c.a, 5);
    CHECK(parseLine("#PBAUD9600", c)); CHECK(c.verb == Verb::Set); CHECK(std::string(c.key) == "BAUD"); CHECK_EQ(c.a, 9600);
    CHECK(parseLine("#PCAL", c)); CHECK(c.verb == Verb::Cal);
    CHECK(parseLine("#PSSID=My Net 5G", c)); CHECK(c.verb == Verb::Set); CHECK(std::string(c.key) == "SSID"); CHECK(!c.hasA); CHECK(std::string(c.text) == "My Net 5G");
    CHECK(parseLine("#PSSID=", c)); CHECK(c.verb == Verb::Set); CHECK(std::string(c.text) == "");
    CHECK(parseLine("#PSSID=2Wire", c)); CHECK(c.verb == Verb::Set); CHECK(!c.hasA); CHECK(std::string(c.text) == "2Wire");
    CHECK(parseLine("#PSSID=12345", c)); CHECK(c.verb == Verb::Set); CHECK(c.hasA); CHECK_EQ(c.a, 12345); CHECK(std::string(c.text) == "12345");
    CHECK(parseLine("#PWIGGLE3x", c)); CHECK(c.verb == Verb::Invalid);
    CHECK(parseLine("#PWIFI", c)); CHECK(c.verb == Verb::Set); CHECK(std::string(c.key) == "WIFI"); CHECK(!c.hasA); CHECK(c.text == nullptr);
    CHECK(parseLine(":PM", c)); CHECK(c.verb == Verb::Random); CHECK(!c.hasA);
    CHECK(parseLine(":PM,80,80,2,4", c)); CHECK(c.verb == Verb::Random); CHECK_EQ(c.a, 80); CHECK_EQ(c.b, 80); CHECK_EQ(c.c, 2); CHECK_EQ(c.d, 4); CHECK(c.hasD);
    CHECK(parseLine(":PWR5", c)); CHECK(c.verb == Verb::Wait); CHECK_EQ(c.b, 1); CHECK_EQ(c.a, 5);
    CHECK(!parseLine("&SABE,HB,1,2,3", c));
    CHECK(!parseLine("hello", c));
    CHECK(!parseLine(":Q1", c));
}

TEST(sequence_canonical_form_accepts_legacy_and_full) {
    char out[256], bad[64];
    CHECK_EQ(canonicalSequence("P100:L5:R30", out, sizeof out, bad, sizeof bad), 3); CHECK(std::string(out) == ":PP100:PL5:PR30");
    CHECK_EQ(canonicalSequence(":PP100:PL5:PR30", out, sizeof out, bad, sizeof bad), 3); CHECK(std::string(out) == ":PP100:PL5:PR30");
    CHECK_EQ(canonicalSequence("PP100:PL5", out, sizeof out, bad, sizeof bad), 2); CHECK(std::string(out) == ":PP100:PL5");
    CHECK_EQ(canonicalSequence("H", out, sizeof out, bad, sizeof bad), 1); CHECK(std::string(out) == ":PH");
    CHECK_EQ(canonicalSequence("X", out, sizeof out, bad, sizeof bad), 1); CHECK(std::string(out) == ":PX");
    CHECK_EQ(canonicalSequence("P100,100:L7:M,80,80,2,4", out, sizeof out, bad, sizeof bad), 3); CHECK(std::string(out) == ":PP100,100:PL7:PM,80,80,2,4");
    CHECK_EQ(canonicalSequence("H:P50:W2:P85,35:A90,25:W2:A270,20,100:W2:P100,100:L5:R50:W4:H", out, sizeof out, bad, sizeof bad), 13);
    CHECK(std::string(out) == ":PH:PP50:PW2:PP85,35:PA90,25:PW2:PA270,20,100:PW2:PP100,100:PL5:PR50:PW4:PH");
    CHECK_EQ(canonicalSequence("W600:P0", out, sizeof out, bad, sizeof bad), 2); CHECK(std::string(out) == ":PW60:PP0");     // waits clamp to 60 s
    CHECK_EQ(canonicalSequence("P100:Q5", out, sizeof out, bad, sizeof bad), -1); CHECK(std::string(bad) == "Q5");
    CHECK_EQ(canonicalSequence("P100:STATE", out, sizeof out, bad, sizeof bad), -1);                                         // config verbs are not chain elements
    CHECK_EQ(canonicalSequence("", out, sizeof out, bad, sizeof bad), 0);
    Command els[8];
    CHECK_EQ(Sequencer::parseChain(":PP100:PL5:PR30", els, 8, nullptr), 3); CHECK(els[1].verb == Verb::Light); CHECK(els[2].verb == Verb::Spin);
    CHECK_EQ(Sequencer::parseChain(":PP100:L5:P50", els, 8, nullptr), 3); CHECK(els[1].verb == Verb::Light); CHECK(els[2].verb == Verb::LiftTo); CHECK_EQ(els[2].a, 50);
    CHECK_EQ(Sequencer::parseChain(":P3P100:P3L5", els, 8, nullptr), 2); CHECK_EQ(els[1].id, 3); CHECK(els[1].verb == Verb::Light);
}

TEST(chain_splitting) {
    const char line[] = ":PP100:PA90,40:PX";
    const char* cursor = line; std::vector<std::string> els; char el[64];
    while (nextChainElement(cursor, el, sizeof el)) els.push_back(el);
    CHECK_EQ(els.size(), 3u);
    CHECK(els[0] == ":PP100"); CHECK(els[1] == ":PA90,40"); CHECK(els[2] == ":PX");
}

TEST(event_formatter_writes_readable_lines) {
    char buf[160]; Event e; e.tMs = 12345; e.code = Ev::JobEnd; e.a = (int)JobKind::SafeLower; e.b = (int)JobResult::Failed; e.c = (int)Reason::HomeFailed;
    formatEvent(e, buf, sizeof buf);
    CHECK(std::string(buf) == "t=12.345 SAFE_LOWER FAILED HOME_FAILED");
    e.code = Ev::SeekClamped; e.a = 0; e.b = 891; e.c = (int)Reason::RotaryNotHome;
    formatEvent(e, buf, sizeof buf);
    CHECK(std::string(buf) == "t=12.345 SEEK CLAMPED 0 -> 891 reason=ROTARY_NOT_HOME");
    e.code = Ev::EstopSettled; e.a = -14; e.b = 0; e.c = 152;
    formatEvent(e, buf, sizeof buf);
    CHECK(std::string(buf) == "t=12.345 ESTOP braked to a stop: lifter -14 ticks, rotary +0 since the latch; drivers off after 152 ms");
    e.c = 2000 | 0x10000; formatEvent(e, buf, sizeof buf);
    CHECK(std::string(buf).find("drivers off after 2000 ms of braking, before the counts settled") != std::string::npos);
    e.code = Ev::Alignment; e.a = 0; e.b = 1118; e.c = 1; formatEvent(e, buf, sizeof buf);
    CHECK(std::string(buf) == "t=12.345 ALIGNMENT above the safe-spin height again at 1118: the head may turn here, and must home before going below that height");
    e.c = 0; formatEvent(e, buf, sizeof buf);
    CHECK(std::string(buf) == "t=12.345 ALIGNMENT ok again (the head is home)");
    e.code = Ev::Measure; e.a = 6; e.b = -19; e.c = -20; formatEvent(e, buf, sizeof buf);
    CHECK(std::string(buf) == "t=12.345 NUDGE came to rest -19 ticks from where it started (asked -20)");
    e.a = 7; e.b = 1103; e.c = 1100; formatEvent(e, buf, sizeof buf);
    CHECK(std::string(buf) == "t=12.345 CLAMP came to rest at 1103, +3 from the safe-spin height 1100");
}
