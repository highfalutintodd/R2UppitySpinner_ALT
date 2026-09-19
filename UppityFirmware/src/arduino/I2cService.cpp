#ifdef ARDUINO
#include <Arduino.h>
#include <Wire.h>
#include "I2cService.h"
#include "../../Board.h"

namespace uppity {

uint8_t I2cService::compose(bool enable) const {
    uint8_t v = 0xFF;                                   // inputs read as 1 (quasi-bidirectional)
    if (light_ & 4) v &= ~(1u << board::kExpLightA); // light kit bits are active low
    if (light_ & 2) v &= ~(1u << board::kExpLightB);
    if (light_ & 1) v &= ~(1u << board::kExpLightC);
    if (enable) { v |= (1u << board::kExpMotorEn); v &= ~(1u << board::kExpMotorEnB); }
    else        { v &= ~(1u << board::kExpMotorEn); v |= (1u << board::kExpMotorEnB); }
    return v;
}

bool I2cService::writeByte(uint8_t v) {
    Wire.beginTransmission(board::kExpanderAddr);
    Wire.write(v);
    bool ok = Wire.endTransmission() == 0;
    if (ok) { shadow_ = v; errors_ = 0; } else ++errors_;
    return ok;
}

bool I2cService::readByte(uint8_t& v) {
    if (Wire.requestFrom(static_cast<int>(board::kExpanderAddr), 1) != 1) { ++errors_; return false; }
    v = static_cast<uint8_t>(Wire.read());
    return true;
}

bool I2cService::begin(Esp32Hal& hal) {
    hal_ = &hal;
    Wire.begin(board::kI2cSda, board::kI2cScl);
    Wire.setTimeOut(5);
    bool ok = writeByte(compose(false));                 // drivers off, lights off, inputs released
    enabledApplied_ = false;
    hal_->setEnableAcked(false);
    return ok;
}

void I2cService::setLightKit(uint8_t mode) { light_ = mode & 7; }

void I2cService::update() {
    if (!hal_) return;
    bool want = hal_->enableRequested();
    uint8_t v = compose(want);
    if (v != shadow_ || want != enabledApplied_) {
        if (writeByte(v)) { enabledApplied_ = want; hal_->setEnableAcked(want); }
        else if (errors_ >= 3) hal_->setEnableAcked(false);
    }
    uint32_t now = millis();
    if (now - lastReadMs_ >= 20) {
        lastReadMs_ = now;
        uint8_t r;
        if (readByte(r)) inputs_ = static_cast<uint8_t>(((r >> board::kExpInputA) & 1 ? 0 : 1) | ((r >> board::kExpInputB) & 1 ? 0 : 2) | ((r >> board::kExpInputC) & 1 ? 0 : 4));
    }
}

} // namespace uppity
#endif
