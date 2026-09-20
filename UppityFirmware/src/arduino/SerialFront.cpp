#ifdef ARDUINO
#include <Arduino.h>
#include "SerialFront.h"

namespace uppity {

void SerialFront::begin(HardwareSerial& port, EstopLatch& estop, EstopLatch::Source src) {
    port_ = &port; estop_ = &estop; src_ = src;
    port_->onReceive([this]() { onRx(); });
}

// UART event task context: scan and copy, nothing else.
void SerialFront::onRx() {
    while (port_->available() > 0) {
        int c = port_->read();
        if (c < 0) break;
        if (scanner_.feed(static_cast<char>(c))) estop_->trip(src_, millis());
        fifo_.push(static_cast<uint8_t>(c));
    }
}

const char* SerialFront::pollLine(LineAssembler::Kind& kind) {
    uint8_t b;
    while (fifo_.pop(b)) {
        LineAssembler::Kind k = assembler_.feed(static_cast<char>(b));
        if (k == LineAssembler::Kind::Command || k == LineAssembler::Kind::Ecosystem) { kind = k; ++lines_; return assembler_.line(); }
    }
    kind = LineAssembler::Kind::None;
    return nullptr;
}

} // namespace uppity
#endif
