#ifdef ARDUINO
#include <Arduino.h>
#include <WiFi.h>
#include <WCB_Client.h>
#include "Mesh.h"
#include "Console.h"
#include "../../Version.h"

namespace uppity {

namespace {
Mesh* gMeshSelf = nullptr;                        // for the library's plain-function callbacks
Mesh::Queue* gMeshLines = nullptr;
EstopLatch* gMeshEstop = nullptr;
std::atomic<uint32_t>* gRx = nullptr;
}

// ---------------------------------------------------------------- bring-up

bool Mesh::begin(const Deps& d) {
    d_ = d;
    const Settings& s = *d_.settings;
    if (!s.meshEnabled) { d_.console->line("MESH=off (#PMESH1 and #PRESTART to join the WCB mesh; or Setup -> Mesh on the page)"); return false; }
    missing_ = meshMissing(s);
    if (missing_) { d_.console->printf("MESH=not started: it still needs %s", missing_); return false; }
    uint8_t oct2 = 0, oct3 = 0;
    parseMeshMac(s.meshMac, oct2, oct3);
    gMeshSelf = this; gMeshLines = d_.lines; gMeshEstop = d_.estop; gRx = &rx_;
    client_ = new WCB_Client(oct2, oct3, s.meshPass, s.meshQty, s.meshId, onCommand, onStatus);
    client_->setMeshChannel(s.meshChannel);
    client_->setChecksum(s.meshCrc != 0);
    client_->setAutoJoin(false);                  // the library would write learned WCBs to flash itself; only Persist writes flash
    client_->setIdentity("Uppity Spinner", UPPITY_FW_VERSION, "v4", "periscope &PERI");   // the WCBs learn this id from it
    client_->enableSpecialPeer(kMeshControllerId); // Sabé: positions and faults go to it directly
    bool ok = client_->begin();
    WiFi.setSleep(false);                         // modem sleep misses ESP-NOW packets
    if (!ok) {
        d_.console->line("MESH=FAILED to start (WCB_Client::begin; its own line above says why). The board runs on without it.");
        delete client_; client_ = nullptr; gMeshSelf = nullptr;
        return false;
    }
    reporter_.start(millis(), s.meshHbS);
    d_.console->printf("MESH=joined as id %u (%u WCBs, octets %s, channel %u, checksum %s); &PERI,HB every %us; the home network is not used while the mesh is on",
                       s.meshId, s.meshQty, s.meshMac, s.meshChannel, s.meshCrc ? "on" : "off", s.meshHbS);
    if (s.meshId > s.meshQty) d_.console->printf("MESH note: id %u is above the WCBs' quantity %u, so they reach it once they have learned it from its adverts (a minute or two after boot)", s.meshId, s.meshQty);
    return true;
}

// ---------------------------------------------------------------- radio task

// Runs in the WiFi task. The latch first, exactly as the serial ports do it, then a copy for loop().
void Mesh::onCommand(uint8_t sender, const char* text) {
    if (!text) return;
    if (gRx) gRx->fetch_add(1);
    if (gMeshSelf) { gMeshSelf->lastRxMs_.store(millis()); gMeshSelf->lastFrom_.store(sender); }
    MeshMsg m = classifyMesh(text);
    if (meshWantsEstop(text)) {
        if (gMeshEstop) gMeshEstop->trip(EstopLatch::Source::Mesh, millis());
        if (gMeshSelf) gMeshSelf->estops_.fetch_add(1);
    }
    if (gMeshLines && m != MeshMsg::Foreign) gMeshLines->push(text, sender);   // other boards' traffic is not even copied
}

void Mesh::onStatus(uint8_t id, bool online) {
    if (!gMeshSelf || id == 0 || id > 31) return;
    uint32_t bit = 1u << id;
    if (online) gMeshSelf->online_.fetch_or(bit); else gMeshSelf->online_.fetch_and(~bit);
}

// ---------------------------------------------------------------- loop()

void Mesh::loop(Ms now) {
    if (!client_) return;
    client_->update();
    char l[208]; uint8_t sender = 0;
    while (d_.lines->pop(l, sizeof l, &sender)) handle(l, sender, now);
    uint32_t on = online_.load();
    if (on != loggedOnline_) {
        for (uint8_t id = 1; id < 32; ++id) {
            uint32_t bit = 1u << id;
            if ((on ^ loggedOnline_) & bit) d_.console->printf("MESH %s%u %s", id == kMeshControllerId ? "controller " : "WCB", id, on & bit ? "online" : "offline");
        }
        loggedOnline_ = on;
    }
    Snapshot s;
    if (d_.pub->read(s)) reporter_.tick(s, now, now / 1000, UPPITY_FW_VERSION, *this);
}

void Mesh::handle(const char* text, uint8_t sender, Ms now) {
    char b[kMeshPacketMax + 1];
    MeshMsg m = classifyMesh(text);
    switch (m) {
        case MeshMsg::Line:
            ++ours_;
            d_.disp->handleLine(text, ReqSource::Mesh, now);      // the same filter as every port: other boards' lines are silent, duplicates run once
            break;
        case MeshMsg::Estop:
            d_.console->printf("MESH &ESTOP from %u: E-STOP latched (release: the page's Reset, #PRESUME, or &ESTOP,RESET)", sender);
            if (formatMeshAck(b, sizeof b, m) > 0) reply(b);
            break;
        case MeshMsg::StopAll:
            d_.console->printf("MESH ?STOP from %u: E-STOP latched", sender);      // the WCBs' own verb: no answer
            break;
        case MeshMsg::EstopReset:
            if (d_.estop->release(now)) d_.console->printf("ESTOP=released (mesh &ESTOP,RESET from %u)", sender);
            if (formatMeshAck(b, sizeof b, m) > 0) reply(b);
            break;
        case MeshMsg::Disable:
            d_.console->stopAll("mesh &DISABLE");
            if (formatMeshAck(b, sizeof b, m) > 0) reply(b);
            break;
        case MeshMsg::Status: {
            Snapshot s;
            if (d_.pub->read(s)) reporter_.burst(s, now, now / 1000, UPPITY_FW_VERSION, *this);
            if (formatMeshAck(b, sizeof b, m) > 0) reply(b);
            break;
        }
        case MeshMsg::Hello:
            if (formatMeshHello(b, sizeof b, UPPITY_FW_VERSION) > 0) reply(b);
            break;
        case MeshMsg::Foreign: default:
            ++foreign_;
            break;
    }
}

void Mesh::reply(const char* text) { broadcast(text); }

bool Mesh::broadcast(const char* text) {
    if (!client_) return false;
    bool ok = client_->broadcast(text, false);    // telemetry: the next one supersedes a lost one
    if (!ok) ++txFail_;
    return ok;
}

bool Mesh::toController(const char* text, bool ensured) {
    if (!client_) return false;
    bool ok = client_->sendToSpecialPeer(text, ensured);
    if (!ok) ++txFail_;
    return ok;
}

// ---------------------------------------------------------------- status

void Mesh::printStatus(TextSink& out) {
    const Settings& s = *d_.settings;
    if (!client_) {
        if (!s.meshEnabled) out.line("MESH=off");
        else out.printf("MESH=not running (%s)", missing_ ? missing_ : "it failed to start, or #PRESTART is still needed");
        return;
    }
    uint32_t on = online_.load(); int wcbs = 0;
    for (uint8_t id = 1; id <= s.meshQty; ++id) if (on & (1u << id)) ++wcbs;
    uint32_t lastRx = lastRxMs_.load();
    out.printf("MESH=on id=%u wcbs=%d/%u online controller=%s neighbours=%u rx=%lu ours=%lu foreign=%lu estops=%lu hb=%lu lastHb=%lus ago lastRx=%s txFail=%lu",
               s.meshId, wcbs, s.meshQty, on & (1u << kMeshControllerId) ? "online" : "not seen", client_->neighborCount(),
               static_cast<unsigned long>(rx_.load()), static_cast<unsigned long>(ours_), static_cast<unsigned long>(foreign_), static_cast<unsigned long>(estops_.load()),
               static_cast<unsigned long>(reporter_.heartbeatsSent()), static_cast<unsigned long>(elapsed(millis(), reporter_.lastHeartbeatMs()) / 1000),
               lastRx ? (String(elapsed(millis(), lastRx) / 1000) + "s ago from " + String(lastFrom_.load())).c_str() : "never", static_cast<unsigned long>(txFail_));
}

void Mesh::writeJson(JsonWriter& j) {
    const Settings& s = *d_.settings;
    j.beginObject();
    j.kv("enabled", s.meshEnabled != 0);
    j.kv("on", client_ != nullptr);
    j.kv("missing", missing_ ? missing_ : "");
    j.kv("id", static_cast<int>(s.meshId)); j.kv("qty", static_cast<int>(s.meshQty)); j.kv("chan", static_cast<int>(s.meshChannel));
    if (client_) {
        uint32_t on = online_.load();
        j.key("wcbs"); j.beginArray();
        for (uint8_t id = 1; id <= s.meshQty; ++id) j.num(static_cast<long>((on >> id) & 1));
        j.endArray();
        j.kv("controller", (on >> kMeshControllerId) & 1 ? true : false);
        j.kv("neighbours", static_cast<int>(client_->neighborCount()));
        j.kv("rx", static_cast<unsigned long>(rx_.load())); j.kv("ours", static_cast<unsigned long>(ours_)); j.kv("estops", static_cast<unsigned long>(estops_.load()));
        j.kv("hb", static_cast<unsigned long>(reporter_.heartbeatsSent()));
        j.kv("lastHbS", static_cast<long>(reporter_.heartbeatsSent() ? elapsed(millis(), reporter_.lastHeartbeatMs()) / 1000 : -1));
        uint32_t lastRx = lastRxMs_.load();
        j.kv("lastRxS", static_cast<long>(lastRx ? elapsed(millis(), lastRx) / 1000 : -1)); j.kv("lastFrom", static_cast<int>(lastFrom_.load()));
        j.kv("txFail", static_cast<unsigned long>(txFail_));
    }
    j.endObject();
}

} // namespace uppity
#endif
