#pragma once
#ifdef ARDUINO
#include "../core/LineAssembler.h"
#include "../core/EstopLatch.h"
// USB console ingestion: the RX callback runs the E-STOP scanner ahead of the FIFO; loop()
// drains the FIFO into the line assembler and hands complete lines to the console.
namespace uppity {
class SerialFront {
  public:
    void begin(EstopLatch& estop);
    // Returns a complete line for us (or nullptr). Call repeatedly from loop().
    const char* pollLine(LineAssembler::Kind& kind);
    uint32_t droppedBytes() const { return fifo_.dropped(); }
    uint32_t droppedLines() const { return assembler_.droppedLines(); }
  private:
    static void onRx();
    static SerialFront* self_;
    EstopLatch* estop_ = nullptr;
    EstopScanner scanner_;
    SerialFifo<512> fifo_;
    LineAssembler assembler_;
};
}
#endif
