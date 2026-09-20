#pragma once
#ifdef ARDUINO
#include <Arduino.h>
#include "../core/LineAssembler.h"
#include "../core/EstopLatch.h"
// One serial source (USB console or the command UART on the Marcduino bus). The RX callback
// runs the E-STOP scanner ahead of the FIFO so ":PX" lands even mid-chain and even while
// loop() is busy; loop() drains the FIFO into this source's own line assembler. Two instances,
// two assemblers, two dispatcher states: a heartbeat on one bus can never splice into a line
// typed on the other (v3's shared-buffer hazard, docs/REWRITE-HANDOFF.md §5.3).
namespace uppity {
class SerialFront {
  public:
    void begin(HardwareSerial& port, EstopLatch& estop, EstopLatch::Source src);
    // Returns a complete line for us (or nullptr). Call repeatedly from loop().
    const char* pollLine(LineAssembler::Kind& kind);
    uint32_t droppedBytes() const { return fifo_.dropped(); }
    uint32_t droppedLines() const { return assembler_.droppedLines(); }
    uint32_t lines() const { return lines_; }
  private:
    void onRx();
    HardwareSerial* port_ = nullptr;
    EstopLatch* estop_ = nullptr;
    EstopLatch::Source src_ = EstopLatch::Source::SerialUsb;
    EstopScanner scanner_;
    SerialFifo<512> fifo_;
    LineAssembler assembler_;
    uint32_t lines_ = 0;
};
}
#endif
