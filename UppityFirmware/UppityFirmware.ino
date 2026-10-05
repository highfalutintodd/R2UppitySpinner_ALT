/*
 * Uppity Spinner ALT v4 — clean-sheet firmware for the Uppity Spinner periscope PCB.
 * https://github.com/highfalutintodd/R2UppitySpinner_ALT
 *
 * Phase 4: motion core + command layer + settings in flash + networking (access point, optional
 * home network, JSON API, log tail, OTA) + the full web UI (Operate, Setup with commissioning,
 * Diagnostics, Rescue with the 60 s override and hold-to-jog). Read docs/REWRITE-PLAN.md and
 * docs/BENCH-PHASE4.md before changing anything here.
 *
 * Licensed under the GNU Lesser General Public License v2.1 (see LICENSE).
 */
#include "Version.h"
#include "Board.h"
#include "src/core/Units.h"
#include "src/core/Hal.h"
#include "src/core/EstopLatch.h"
#include "src/core/CommandQueue.h"
#include "src/core/StatusSnapshot.h"
#include "src/core/EventLog.h"
#include "src/core/EventFormat.h"
#include "src/core/MotionSupervisor.h"
#include "src/core/Sequencer.h"
#include "src/core/RandomDirector.h"
#include "src/core/Dispatcher.h"
#include "src/arduino/Esp32Hal.h"
#include "src/arduino/SupervisorTask.h"
#include "src/arduino/I2cService.h"
#include "src/arduino/SerialFront.h"
#include "src/arduino/Persist.h"
#include "src/arduino/Console.h"
#include "src/arduino/StatusLed.h"
#include "src/arduino/Net.h"
#include "src/core/TextRing.h"
#include "src/core/LineQueue.h"

using namespace uppity;

// Spinlock for the command queue: producers in loop(), consumer in the control task.
struct MuxLock final : Lock {
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    void lock() override { portENTER_CRITICAL(&mux); }
    void unlock() override { portEXIT_CRITICAL(&mux); }
};

static Esp32Hal          gHal;
static EstopLatch        gEstop;
static MuxLock           gLock;
static CommandQueue      gQueue(gLock);
static SnapshotPublisher gSnapshot;
static EventLog          gEvents;
static MotionSupervisor  gSup(gHal, gEstop, gQueue, gSnapshot, gEvents);
static SupervisorTask    gTask;
static I2cService        gI2c;
static SerialFront       gUsb, gCmd;
static Persist           gPersist;
static Sequencer         gSeq;
static RandomDirector    gDir;
static Dispatcher        gDisp;
static Console           gConsole;
static StatusLed         gLed;
static MuxLock           gLogLock, gWebLock;
static LogRing           gLog(gLogLock);
static WebLineQueue      gWebLines(gWebLock);
static Net               gNet;

struct NetStatusAdapter final : Console::NetStatus {
    void printStatus(TextSink& out) override { gNet.printStatus(out); }
    bool anyClient() const override { return gNet.anyClient(); }
} gNetStatus;

static void panicHook(void*) { gHal.panicStop(); }

static void drainEvents() {
    Event e; char line[160];
    while (gEvents.pop(e)) {
        if (e.code == Ev::LightKit) gI2c.setLightKit(static_cast<uint8_t>(e.a));
        if (e.code == Ev::Estop) gConsole.noteEstop(e.b != 0);
        gConsole.onEvent(e);                                                        // commissioning watches these; hints for common failures
        if (e.code == Ev::Tick) {                                                   // quiet unless the tick is stressed
            static int32_t lastLate = 0;
            bool newLate = e.b != lastLate; lastLate = e.b;
            if (!newLate && e.a < 500 && !gConsole.settings().debug) continue;
        }
        formatEvent(e, line, sizeof line);
        gConsole.line(line);
    }
    uint32_t d = gEvents.dropped();
    static uint32_t lastDropped = 0;
    if (d != lastDropped) { gConsole.printf("EVENTS DROPPED=%lu", (unsigned long)(d - lastDropped)); lastDropped = d; }
}

void setup() {
    // A TX buffer, so a long print (#PCONFIG is ~850 bytes, ~60 ms at 115200) never holds up loop(),
    // which switches the drivers on for the supervisor (code review 2026-10, finding 14).
    Serial.setTxBufferSize(1024);
    Serial.begin(115200);
    delay(200);
    Console::Deps d;
    d.sup = &gSup; d.q = &gQueue; d.estop = &gEstop; d.pub = &gSnapshot; d.i2c = &gI2c; d.persist = &gPersist;
    d.seq = &gSeq; d.disp = &gDisp; d.dir = &gDir; d.hal = &gHal; d.usb = &Serial; d.cmd = &Serial2; d.log = &gLog; d.web = &gWebLines; d.cmdFront = &gCmd;
    gConsole.begin(d);                                 // from here on every line also lands in the log ring
    Serial.println();
    gConsole.printf("Uppity Spinner ALT v%s  phase 4  build %s", UPPITY_FW_VERSION, Console::buildId());
    gEstop.setPanic(panicHook, nullptr);
    bool halOk = gHal.begin();
    gConsole.printf("HAL=%s%s", halOk ? "ok" : "FAILED ", halOk ? "" : gHal.lastError());
    bool i2cOk = gI2c.begin(gHal);
    gConsole.printf("EXPANDER=%s", i2cOk ? "ok (drivers off, lights off)" : "NOT RESPONDING (motors cannot be enabled)");
    gLed.begin();

    gSeq.begin(gQueue, gSnapshot, gPersist.sequences(), gConsole.rng(), gConsole, gConsole);
    gDir.begin(gConsole.rng(), gConsole);
    gDisp.begin(gSeq, gConsole, gConsole);
    gConsole.boot();                                   // settings loaded (v3 imported once), applied, command UART open

    gSup.begin(millis());
    gUsb.begin(Serial, gEstop, EstopLatch::Source::SerialUsb);
    gCmd.begin(Serial2, gEstop, EstopLatch::Source::SerialCmd);
    // The radio comes up before the 1 kHz control task: WiFi bring-up stalls both cores for tens of
    // milliseconds (flash access with the cache off) and would otherwise show as late ticks. The
    // E-STOP latch and its panic hook already exist, and nothing can move until the task runs.
    Net::Deps nd; nd.console = &gConsole; nd.settings = &gConsole.settings(); nd.pub = &gSnapshot; nd.estop = &gEstop; nd.sup = &gSup; nd.lines = &gWebLines; nd.log = &gLog; nd.q = &gQueue;
    gNet.begin(nd);
    gConsole.setNet(&gNetStatus);
    bool taskOk = gTask.start(gSup);
    gConsole.printf("CONTROL_TASK=%s", taskOk ? "running core1 prio10 1kHz" : "FAILED");
    gConsole.line("READY (type #PHELP)");
    drainEvents();
}

static void pollSerial(SerialFront& front, ReqSource src) {
    LineAssembler::Kind kind;
    const char* line;
    while ((line = front.pollLine(kind)) != nullptr) {
        if (kind == LineAssembler::Kind::Ecosystem) gConsole.handleEcosystemLine(line);
        else gDisp.handleLine(line, src, millis());
    }
}

void loop() {
    uint32_t now = millis();
    pollSerial(gUsb, ReqSource::SerialUsb);
    pollSerial(gCmd, ReqSource::SerialCmd);
    { char l[256]; while (gWebLines.pop(l, sizeof l)) gConsole.handleWebLine(l); }
    gNet.otaProgressToLog();
    gSeq.tick(now);
    gConsole.update(now);
    gI2c.update();
    drainEvents();
    { Snapshot s; if (gSnapshot.read(s)) gLed.update(s, gConsole.randomActive(), gNet.anyClient(), now); }
    delay(1);
}
