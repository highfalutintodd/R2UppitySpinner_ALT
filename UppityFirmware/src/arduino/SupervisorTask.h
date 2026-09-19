#pragma once
#ifdef ARDUINO
#include "../core/MotionSupervisor.h"
// The 1 kHz control task: core 1, priority 10, vTaskDelayUntil. Only this task calls tick().
namespace uppity {
class SupervisorTask {
  public:
    bool start(MotionSupervisor& sup);
    bool running() const { return handle_ != nullptr; }
  private:
    static void taskFn(void* arg);
    MotionSupervisor* sup_ = nullptr;
    void* handle_ = nullptr;
};
}
#endif
