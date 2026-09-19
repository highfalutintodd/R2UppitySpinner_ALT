/*
 * Uppity Spinner ALT v4 — clean-sheet firmware for the Uppity Spinner periscope PCB.
 * https://github.com/highfalutintodd/R2UppitySpinner_ALT
 *
 * Phase 1: motion core + USB console. No WiFi, no persistence yet.
 * Read docs/REWRITE-PLAN.md and docs/BENCH-PHASE1.md before changing anything here.
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
#include "src/arduino/Esp32Hal.h"
#include "src/arduino/SupervisorTask.h"
#include "src/arduino/I2cService.h"
#include "src/arduino/SerialFront.h"
#include "src/arduino/Console.h"

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
static SerialFront       gSerial;
static Console           gConsole;

static void panicHook(void*) { gHal.panicStop(); }

static void drainEvents() {
    Event e; char line[160];
    while (gEvents.pop(e)) {
        if (e.code == Ev::LightKit) { gI2c.setLightKit(static_cast<uint8_t>(e.a)); }
        if (e.code == Ev::Tick && e.b == 0 && e.a < 500) continue;          // quiet unless the tick is stressed
        formatEvent(e, line, sizeof line);
        Serial.println(line);
    }
    uint32_t d = gEvents.dropped();
    static uint32_t lastDropped = 0;
    if (d != lastDropped) { Serial.printf("EVENTS DROPPED=%lu\n", (unsigned long)(d - lastDropped)); lastDropped = d; }
}

static void updateStatusLed(const Snapshot& s, uint32_t now) {
    bool blink = (now / 250) & 1;
    uint8_t r = 0, g = 0, b = 0;
    switch (s.state) {
        case MotionState::Locked:  r = 2; g = 2; break;                                   // yellow: commissioning needed
        case MotionState::Stopped: r = blink ? 6 : 0; break;                              // fast red blink: E-STOP latched
        case MotionState::Fault:   if (blink) r = 4; else { r = 2; g = 2; } break;        // red / yellow
        case MotionState::Moving:  if (blink) g = 3; else { r = 2; g = 2; } break;        // green / yellow
        default:
            if (s.homeConf == HomeConfidence::Latched || s.homeConf == HomeConfidence::SwitchLive || s.rotaryDisabled) g = 2; else r = 2;
            break;
    }
    rgbLedWrite(board::kStatusLed, r, g, b);
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.printf("\nUppity Spinner ALT v%s  phase 1 (%s %s)\n", UPPITY_FW_VERSION, __DATE__, __TIME__);
    gEstop.setPanic(panicHook, nullptr);
    bool halOk = gHal.begin();
    Serial.printf("HAL=%s%s\n", halOk ? "ok" : "FAILED ", halOk ? "" : gHal.lastError());
    bool i2cOk = gI2c.begin(gHal);
    Serial.printf("EXPANDER=%s\n", i2cOk ? "ok (drivers off, lights off)" : "NOT RESPONDING (motors cannot be enabled)");
    gConsole.begin(gSup, gQueue, gEstop, gI2c, Serial);
    MotionConfig& cfg = gConsole.config();
    cfg.motor = LifterMotor::Pololu4757_6p3;
    cfg.commissioned = false;                                  // locked until #PUNLOCK (phase 1) or commissioning (phase 4)
    cfg.rotary.driveSign = -1;                                 // v3 wiring convention; #PROTARYTEST will measure it in phase 2
    gSup.setConfig(cfg);
    gSup.begin(millis());
    gSerial.begin(gEstop);
    bool taskOk = gTask.start(gSup);
    Serial.printf("CONTROL_TASK=%s\nREADY (type #PHELP)\n", taskOk ? "running core1 prio10 1kHz" : "FAILED");
    drainEvents();
}

void loop() {
    static uint32_t lastLed = 0;
    LineAssembler::Kind kind;
    const char* line;
    while ((line = gSerial.pollLine(kind)) != nullptr) gConsole.handleLine(line, kind);
    gI2c.update();
    drainEvents();
    uint32_t now = millis();
    if (now - lastLed >= 100) { lastLed = now; Snapshot s; if (gSnapshot.read(s)) updateStatusLed(s, now); }
    delay(1);
}
