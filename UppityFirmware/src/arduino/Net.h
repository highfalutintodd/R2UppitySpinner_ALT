#pragma once
#ifdef ARDUINO
#include <Arduino.h>
#include <atomic>
#include <WebServer.h>
#include <DNSServer.h>
#include "../core/Settings.h"
#include "../core/StatusSnapshot.h"
#include "../core/EstopLatch.h"
#include "../core/MotionSupervisor.h"
#include "../core/LineQueue.h"
#include "../core/TextRing.h"
#include "../core/TextSink.h"
#include "../core/Json.h"
// Networking (docs/REWRITE-PLAN.md §3.6), mirroring Sabé's C6: the access point comes up ALWAYS
// and first (the path that cannot be typo'd away), the home network is joined in the background
// with our own backoff and never while a phone is on the AP, captive DNS runs only while the
// station is down, mDNS is registered after ArduinoOTA (which re-inits it), assets are gzipped
// PROGMEM with an ETag keyed to the page hash, and no password is ever sent to a browser.
//
// The web server runs in its own task on core 0 so a slow phone download never delays loop().
// Handlers touch shared state only through the safe paths: the status snapshot (seqlock), the
// E-STOP latch (atomic, trips from any context), the line queue into loop() (locked) and the
// log ring (locked). Nothing here drives a motor.
//
// E-STOP has its own listener on port 81 in its own task (bench 2026-09-27: the page's E-STOP
// never landed while the lifter ran; the radio loses packets around lifter moves and the
// single-threaded web server waits up to 5 s on a connection whose request never arrives).
// The page hits both ports and keeps retrying until the status confirms the latch. The main
// server also drops a connection that has sent nothing for 400 ms instead of the library's 5 s.
namespace uppity {

class Console;

class Net {
  public:
    struct Deps {
        Console* console = nullptr; Settings* settings = nullptr; SnapshotPublisher* pub = nullptr;
        EstopLatch* estop = nullptr; MotionSupervisor* sup = nullptr; WebLineQueue* lines = nullptr; LogRing* log = nullptr;
        CommandQueue* q = nullptr;                    // only for the jog lease (an atomic): the jog itself starts as a #PJOG line
    };
    bool begin(const Deps& d);                    // radio, routes, task; false = WiFi disabled in settings
    bool up() const { return up_; }
    bool staConnected() const { return staState_.load() == 2; }
    uint8_t apClients() const { return apClients_.load(); }
    bool anyClient() const { return apClients() > 0 || staConnected(); }
    const char* apSsid() const { return apSsid_; }
    void printStatus(TextSink& out);
    void writeWifiJson(JsonWriter& j);
    void otaProgressToLog();                      // called from loop()

  private:
    static void taskFn(void* arg);
    static void estopTaskFn(void* arg);
    void service();
    void serviceEstopPort();
    void dropStalledClient();
    void serviceSta();
    void setPortal(bool want);
    void registerRoutes();
    void setupOta();
    void sendJson(int code, const char* body);
    void ok(const char* msg = nullptr);
    void fail(int code, const char* err, const char* msg = nullptr);
    bool disarmed();
    void handleState();
    void handleCmd();
    void handleConfigGet();
    void handleConfigPost();
    void handleLog();
    void handleJog();
    void handleUpdateDone();
    void handleUpdateUpload();

    Deps d_;
    WebServer server_{80};
    NetworkServer estopServer_{81};
    DNSServer dns_;
    char staSsid_[33] = {};                       // credentials as they were at boot: a change waits for #PRESTART
    char staPass_[65] = {};
    int curFd_ = -1; uint32_t curFdSince_ = 0; bool handledSince_ = false;
    uint32_t stalledDropped_ = 0, estopHits_ = 0, webEstops_ = 0;
    char apSsid_[24] = "R2Uppity";
    bool up_ = false, portalUp_ = false, staPauseLogged_ = false, meshOn_ = false;
    bool apDefault_ = false;                      // the access point runs on the published default password
    std::atomic<uint8_t> staState_{0};            // 0 off/idle, 1 joining, 2 connected
    std::atomic<bool> staEvent_{false};
    std::atomic<uint8_t> staReason_{0};
    std::atomic<uint8_t> apClients_{0};
    std::atomic<int> otaPct_{-1};
    int otaLoggedPct_ = -1;
    uint32_t staNextTryMs_ = 0, staBackoffMs_ = 15000;
    bool otaRefused_ = false, otaFailed_ = false;
    char json_[3072];
    char logBuf_[4096];
    char logJson_[6144];
    void* task_ = nullptr;
    void* estopTask_ = nullptr;
};

} // namespace uppity
#endif
