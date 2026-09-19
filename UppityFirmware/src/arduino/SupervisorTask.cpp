#ifdef ARDUINO
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include "SupervisorTask.h"

namespace uppity {

bool SupervisorTask::start(MotionSupervisor& sup) {
    sup_ = &sup;
    TaskHandle_t h = nullptr;
    BaseType_t ok = xTaskCreatePinnedToCore(taskFn, "uppity_ctrl", 8192, this, 10, &h, 1);
    handle_ = ok == pdPASS ? h : nullptr;
    return handle_ != nullptr;
}

void SupervisorTask::taskFn(void* arg) {
    SupervisorTask* self = static_cast<SupervisorTask*>(arg);
    esp_task_wdt_add(nullptr);                       // no-op error if the TWDT is not initialised
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        uint32_t t0 = micros();
        self->sup_->tick(millis());
        uint32_t dt = micros() - t0;
        BaseType_t delayed = xTaskDelayUntil(&last, 1);   // pdFALSE = we were already late
        self->sup_->reportTickStats(dt, delayed == pdFALSE);
        esp_task_wdt_reset();
    }
}

} // namespace uppity
#endif
