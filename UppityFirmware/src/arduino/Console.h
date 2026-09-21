#pragma once
#ifdef ARDUINO
#include <Arduino.h>
#include "../core/Parser.h"
#include "../core/MotionSupervisor.h"
#include "../core/CommandQueue.h"
#include "../core/EstopLatch.h"
#include "../core/Settings.h"
#include "../core/Sequencer.h"
#include "../core/RandomDirector.h"
#include "../core/Dispatcher.h"
#include "../core/TextSink.h"
#include "../core/Json.h"
#include "I2cService.h"
#include "Persist.h"
#include "Esp32Hal.h"
#include "../core/TextRing.h"
#include "../core/LineQueue.h"
// The operator's side of the firmware: owns the settings and the calibration in RAM, answers
// every "#P" command, feeds the random director and the trigger inputs into the dispatcher,
// pulls calibration/characterisation results back out of the supervisor so they persist, and
// prints everything as KEY=VALUE lines on the USB console (and the command UART if #PCMDTX1).
namespace uppity {

struct EspRandom final : RandomSource { uint32_t next() override; };

class Console final : public TextSink, public Sequencer::Listener, public Dispatcher::ConfigHandler {
  public:
    struct Deps {
        MotionSupervisor* sup = nullptr; CommandQueue* q = nullptr; EstopLatch* estop = nullptr; SnapshotPublisher* pub = nullptr;
        I2cService* i2c = nullptr; Persist* persist = nullptr; Sequencer* seq = nullptr; Dispatcher* disp = nullptr;
        RandomDirector* dir = nullptr; Esp32Hal* hal = nullptr; HardwareSerial* usb = nullptr; HardwareSerial* cmd = nullptr;
        LogRing* log = nullptr; WebLineQueue* web = nullptr;
    };
    class NetStatus { public: virtual ~NetStatus() = default; virtual void printStatus(TextSink&) = 0; virtual bool anyClient() const = 0; };
    void begin(const Deps& d);
    void boot();                                   // load settings, one-shot v3 import, apply; before the supervisor starts
    void update(Ms now);                           // every loop()
    void handleEcosystemLine(const char* line);
    void handleWebLine(const char* line);        // from the web line queue: "!STOP" or a :P/#P line
    void setNet(NetStatus* n) { net_ = n; }
    void writeStateJson(JsonWriter& j);          // the console's part of /api/state (any task; plain reads)
    void noteEstop(bool latched);
    Settings& settings() { return s_; }
    const Calibration& calibration() const { return cal_; }
    bool randomActive() const;
    RandomSource& rng() { return rng_; }
    // TextSink
    void line(const char* text) override;
    // Sequencer::Listener
    void onLight(int mode) override;
    void onRandom(const Command& c) override;
    void onRandomStop() override;
    void onEstop() override;
    // Dispatcher::ConfigHandler
    void onConfig(const Command& c, const char* line, ReqSource src) override;

    void printState(); void printConfig(); void printCal(); void printStatus(); void printHelp(); void printV3(); void printSequences();
  private:
    void applySettings(bool boot);
    void setKey(const Command& c);
    void importV3Now(bool forced);
    void pushOperatorJob(JobKind k, int32_t a);
    void stopRandom(const char* why);
    bool refuseWhileMoving(const char* what);
    RandomDirector::Limits limits() const;
    void beginCmdSerial();
    Deps d_;
    Settings s_; Calibration cal_;
    EspRandom rng_;
    TriggerInputs triggers_; uint32_t lastInputReads_ = 0;
    bool restartPending_ = false; Ms restartAt_ = 0;
    NetStatus* net_ = nullptr;
    Ms bootMs_ = 0;
};

} // namespace uppity
#endif
