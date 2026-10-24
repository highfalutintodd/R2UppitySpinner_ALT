#pragma once
#ifdef ARDUINO
#include <Arduino.h>
#include <atomic>
#include "../core/MeshProtocol.h"
#include "../core/Settings.h"
#include "../core/EstopLatch.h"
#include "../core/LineQueue.h"
#include "../core/StatusSnapshot.h"
#include "../core/Dispatcher.h"
#include "../core/TextSink.h"
#include "../core/Json.h"
#include "Console.h"
class WCB_Client;
// The WCB mesh (docs/REWRITE-PLAN.md §1.1, Appendix C; docs/PHASE5-BRIEF.md): off by default.
// When on, the periscope is one more device on Greg Hulette's ESP-NOW mesh, with its own id.
//   * The radio task (WCB_Client's receive callback) does two things only: trips the E-STOP latch
//     for "&ESTOP", "?STOP", ":PX" or "&SABE,ESTOP" (the same patterns as the serial ports), and
//     copies the line into a queue. Everything else happens in loop().
//   * loop(): ":P"/"#P" lines go to the dispatcher as ReqSource::Mesh (unattended: never the rescue
//     override or the commissioning unlock; #P limited by meshMayRunConfig()); the "&" verbs are
//     answered; the MeshReporter sends the "&PERI,..." heartbeat, position, state and faults.
//   * WCB_Client's own auto-join is off, so the library never writes flash (hard rule 5); the WCBs
//     still learn this board from its discovery adverts (setIdentity), which is what makes an id
//     above their quantity reachable.
namespace uppity {

class Mesh final : public MeshReporter::Out, public Console::MeshStatus {
  public:
    static constexpr int kLineMax = 208;          // a longer message is dropped whole (WCB_Client reassembles up to ~2.8 KB)
    using Queue = LineQueue<16, kLineMax>;
    struct Deps {
        Console* console = nullptr; const Settings* settings = nullptr; EstopLatch* estop = nullptr;
        Dispatcher* disp = nullptr; SnapshotPublisher* pub = nullptr; Queue* lines = nullptr;
    };
    bool begin(const Deps& d);                    // after Net::begin (the access point must already be on the mesh channel)
    void loop(Ms now);
    bool running() const { return client_ != nullptr; }
    void printStatus(TextSink& out) override;
    void writeJson(JsonWriter& j) override;
    // MeshReporter::Out
    bool broadcast(const char* text) override;
    bool toController(const char* text, bool ensured) override;

  private:
    static void onCommand(uint8_t sender, const char* text);   // radio task
    static void onStatus(uint8_t id, bool online);              // radio task
    void handle(const char* text, uint8_t sender, Ms now);
    void reply(const char* text);
    void sendPaced(Ms now);

    Deps d_;
    WCB_Client* client_ = nullptr;
    MeshReporter reporter_;
    MeshTxPacer txPacer_;                         // the board's own broadcasts, in order and spaced
    bool failed_ = false;                         // WCB_Client::begin failed at boot
    std::atomic<uint32_t> rx_{0}, estops_{0}, online_{0}, dropped_{0};
    std::atomic<uint32_t> lastRxMs_{0};
    std::atomic<uint8_t> lastFrom_{0};
    uint32_t loggedOnline_ = 0, txFail_ = 0, ours_ = 0, foreign_ = 0, refused_ = 0;
};

} // namespace uppity
#endif
