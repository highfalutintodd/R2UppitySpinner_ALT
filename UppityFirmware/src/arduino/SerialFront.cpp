#ifdef ARDUINO
#include <Arduino.h>
#include "SerialFront.h"

namespace uppity {

SerialFront* SerialFront::self_ = nullptr;

void SerialFront::begin(EstopLatch& estop) {
    estop_ = &estop;
    self_ = this;
    Serial.onReceive(onRx);
}

// UART event task context: scan and copy, nothing else.
void SerialFront::onRx() {
    SerialFront* s = self_;
    if (!s) return;
    while (Serial.available() > 0) {
        int c = Serial.read();
        if (c < 0) break;
        if (s->scanner_.feed(static_cast<char>(c))) s->estop_->trip(EstopLatch::Source::SerialUsb, millis());
        s->fifo_.push(static_cast<uint8_t>(c));
    }
}

const char* SerialFront::pollLine(LineAssembler::Kind& kind) {
    uint8_t b;
    while (fifo_.pop(b)) {
        LineAssembler::Kind k = assembler_.feed(static_cast<char>(b));
        if (k == LineAssembler::Kind::Command || k == LineAssembler::Kind::Ecosystem) { kind = k; return assembler_.line(); }
    }
    kind = LineAssembler::Kind::None;
    return nullptr;
}

} // namespace uppity
#endif
