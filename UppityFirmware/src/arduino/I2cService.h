#pragma once
#ifdef ARDUINO
#include <cstdint>
#include "Esp32Hal.h"
// Sole owner of Wire and the PCF8574 expander (Board.h bit map). Runs from loop():
// applies the supervisor's enable request (EN high / ENB low = drivers on), the light kit
// mode bits, and reads the three external trigger inputs. Never called from the tick.
namespace uppity {
class I2cService {
  public:
    bool begin(Esp32Hal& hal);
    void update();                              // call every loop()
    void setLightKit(uint8_t mode);             // 0..7, applied on the next update
    uint8_t lightKit() const { return light_; }
    uint8_t inputs() const { return inputs_; }  // bit0 A, bit1 B, bit2 C (1 = grounded)
    bool ok() const { return errors_ < 3; }
    uint32_t errors() const { return errors_; }
    bool enabledApplied() const { return enabledApplied_; }
  private:
    bool writeByte(uint8_t v);
    bool readByte(uint8_t& v);
    uint8_t compose(bool enable) const;
    Esp32Hal* hal_ = nullptr;
    uint8_t light_ = 1;                         // off
    uint8_t shadow_ = 0xFF;
    uint8_t inputs_ = 0;
    bool enabledApplied_ = false;
    uint32_t errors_ = 0, lastReadMs_ = 0;
};
}
#endif
