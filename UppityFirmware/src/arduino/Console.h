#pragma once
#ifdef ARDUINO
#include <Arduino.h>
#include "../core/Parser.h"
#include "../core/LineAssembler.h"
#include "../core/MotionSupervisor.h"
#include "../core/CommandQueue.h"
#include "../core/EstopLatch.h"
#include "I2cService.h"
// Phase 1 console: turns parsed ":P"/"#P" lines into motion requests and prints KEY=VALUE
// answers. Configuration lives in RAM until Phase 2 adds persistence.
namespace uppity {
class Console {
  public:
    void begin(MotionSupervisor& sup, CommandQueue& q, EstopLatch& estop, I2cService& i2c, Print& out);
    void handleLine(const char* line, LineAssembler::Kind kind);
    MotionConfig& config() { return cfg_; }
    void printState();
    void printConfig();
    void printV3Prefs();
  private:
    void handleCommand(const Command& c);
    bool push(JobKind k, int32_t a, int32_t b, int32_t c);
    Ticks travel() const;
    int32_t rnd(int32_t lo, int32_t hi) const;
    MotionSupervisor* sup_ = nullptr;
    CommandQueue* q_ = nullptr;
    EstopLatch* estop_ = nullptr;
    I2cService* i2c_ = nullptr;
    Print* out_ = nullptr;
    MotionConfig cfg_;
};
}
#endif
