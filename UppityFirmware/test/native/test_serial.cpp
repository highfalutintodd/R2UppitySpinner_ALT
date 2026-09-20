#include "uppity_test.h"
#include "../../src/core/LineAssembler.h"
#include "../../src/core/Parser.h"
#include "../../src/core/EventFormat.h"
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
    CHECK(parseLine("#PMOTOR1", c)); CHECK(c.verb == Verb::Motor); CHECK_EQ(c.a, 1);
    CHECK(parseLine("#PINVERT1", c)); CHECK(c.verb == Verb::Invert); CHECK(c.hasA); CHECK_EQ(c.a, 1);
    CHECK(parseLine("#PINVERT", c)); CHECK(c.verb == Verb::Invert); CHECK(!c.hasA);
    CHECK(parseLine("#PTRAVEL1150", c)); CHECK(c.verb == Verb::Travel); CHECK_EQ(c.a, 1150);
    CHECK(parseLine("#PSAFESPIN760", c)); CHECK(c.verb == Verb::SafeSpin); CHECK_EQ(c.a, 760);
    CHECK(parseLine("#PV3", c)); CHECK(c.verb == Verb::V3Dump);
    CHECK(parseLine("#PHOMEOFFSET-18", c)); CHECK(c.verb == Verb::HomeOffset); CHECK_EQ(c.a, -18);
    CHECK(parseLine("#PROTARYTPR11855", c)); CHECK(c.verb == Verb::RotaryTpr); CHECK_EQ(c.a, 11855);
    CHECK(parseLine("#PROTARYTEST3", c)); CHECK(c.verb == Verb::RotaryTest); CHECK_EQ(c.a, 3);
    CHECK(parseLine("#PWIGGLE2", c)); CHECK(c.verb == Verb::Wiggle); CHECK_EQ(c.a, 2);
    CHECK(parseLine("#PCLEARFAULT", c)); CHECK(c.verb == Verb::ClearFault);
    CHECK(parseLine("#PBOGUS", c)); CHECK(c.verb == Verb::Invalid);
    CHECK(!parseLine("&SABE,HB,1,2,3", c));
    CHECK(!parseLine("hello", c));
    CHECK(!parseLine(":Q1", c));
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
}
