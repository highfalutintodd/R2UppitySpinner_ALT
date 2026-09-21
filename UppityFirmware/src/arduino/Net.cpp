#ifdef ARDUINO
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include "Net.h"
#include "Console.h"
#include "WebAssets.h"
#include "../core/EventFormat.h"
#include "../../Version.h"

namespace uppity {

namespace {
const char* kHost = "uppity";                     // http://uppity.local, and the ArduinoOTA name (must match: OTA re-inits mDNS)
Net* gNetSelf = nullptr;                          // for the WiFi event callback
}

// ---------------------------------------------------------------- bring-up

bool Net::begin(const Deps& d) {
    d_ = d;
    gNetSelf = this;
    if (!d_.settings->wifiEnabled) { d_.console->line("WIFI=off (#PWIFI1 and #PRESTART to enable)"); return false; }
    uint8_t mac[6]; WiFi.macAddress(mac);
    std::snprintf(apSsid_, sizeof apSsid_, "R2Uppity-%02X%02X", mac[4], mac[5]);     // v3's hostname convention
    bool sta = d_.settings->staSsid[0] != '\0';
    WiFi.persistent(false);
    WiFi.mode(sta ? WIFI_AP_STA : WIFI_AP);
    WiFi.setHostname(kHost);
    WiFi.setSleep(false);
    WiFi.softAP(apSsid_, d_.settings->apPass, d_.settings->apChannel);
    bool defaultPass = std::strcmp(d_.settings->apPass, "Astromech") == 0;
    d_.console->printf("WIFI AP \"%s\" password %s -> http://%s  (http://%s.local)", apSsid_, defaultPass ? "\"Astromech\" (the default; #PAPPASS=<yours> to change)" : "<yours>", WiFi.softAPIP().toString().c_str(), kHost);
    WiFi.onEvent([](WiFiEvent_t ev, WiFiEventInfo_t info) {
        Net* n = gNetSelf; if (!n) return;
        switch (ev) {
            case ARDUINO_EVENT_WIFI_STA_GOT_IP: n->staState_.store(2); n->staEvent_.store(true); break;
            case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: n->staReason_.store(info.wifi_sta_disconnected.reason); if (n->staState_.load() != 0) { n->staState_.store(0); n->staEvent_.store(true); } break;
            case ARDUINO_EVENT_WIFI_AP_STACONNECTED: case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED: n->apClients_.store(static_cast<uint8_t>(WiFi.softAPgetStationNum())); break;
            default: break;
        }
    });
    if (sta) {
        WiFi.setAutoReconnect(false);             // our own schedule (Sabé): stock reconnect hammers a missing network and disturbs the AP
        staNextTryMs_ = millis();                 // first try at once
        d_.console->printf("WIFI home network \"%s\" stored; joining in the background", d_.settings->staSsid);
    } else d_.console->line("WIFI no home network stored (#PSSID=<name> #PPASS=<password> #PRESTART to join one)");
    setPortal(true);
    MDNS.begin(kHost);
    setupOta();                                   // BEFORE addService: ArduinoOTA::begin() re-runs MDNS.begin()
    MDNS.addService("http", "tcp", 80);
    const char* wanted[] = { "If-None-Match" };
    server_.collectHeaders(wanted, 1);
    registerRoutes();
    server_.begin();
    up_ = true;
    TaskHandle_t h = nullptr;
    xTaskCreatePinnedToCore(taskFn, "uppity_net", 8192, this, 1, &h, 0);
    task_ = h;
    return true;
}

void Net::taskFn(void* arg) {
    Net* n = static_cast<Net*>(arg);
    for (;;) { n->service(); vTaskDelay(2); }
}

void Net::service() {
    if (portalUp_) dns_.processNextRequest();
    server_.handleClient();
    serviceSta();
    if (disarmed()) ArduinoOTA.handle();          // the IDE/CLI network port answers only while nothing can move
}

bool Net::disarmed() {
    Snapshot s; if (!d_.pub->read(s)) return false;
    return !s.enabled && s.activeJob == JobKind::None;
}

// ---------------------------------------------------------------- station (home network)

void Net::setPortal(bool want) {
    if (want == portalUp_) return;
    if (want) { dns_.setErrorReplyCode(DNSReplyCode::NoError); dns_.start(53, "*", WiFi.softAPIP()); }
    else dns_.stop();
    portalUp_ = want;
}

void Net::serviceSta() {
    const Settings& s = *d_.settings;
    if (s.staSsid[0] == '\0') return;
    if (staEvent_.exchange(false)) {
        if (staState_.load() == 2) {
            staBackoffMs_ = 15000;
            d_.console->printf("WIFI joined \"%s\": http://%s.local  (%s, %d dBm); the AP stays up too", s.staSsid, kHost, WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
        } else {
            d_.console->printf("WIFI not on \"%s\" (reason %u); retry in %lus, the AP is still up", s.staSsid, (unsigned)staReason_.load(), (unsigned long)(staBackoffMs_ / 1000));
            staNextTryMs_ = millis() + staBackoffMs_;
            staBackoffMs_ = staBackoffMs_ >= 300000 ? 300000 : staBackoffMs_ * 2;
        }
    }
    setPortal(staState_.load() != 2);
    if (staState_.load() == 2) return;
    if (static_cast<int32_t>(millis() - staNextTryMs_) < 0) return;
    if (WiFi.softAPgetStationNum() > 0) {         // somebody is using the AP: a scan would disturb them
        if (!staPauseLogged_) { staPauseLogged_ = true; d_.console->line("WIFI home-network retry paused while a phone is on the AP"); }
        staNextTryMs_ = millis() + 30000;
        return;
    }
    staPauseLogged_ = false;
    staState_.store(1);
    WiFi.begin(s.staSsid, s.staPass[0] ? s.staPass : nullptr);
    staNextTryMs_ = millis() + (staBackoffMs_ > 20000 ? staBackoffMs_ : 20000);   // a wedged join cannot wait forever
}

// ---------------------------------------------------------------- OTA (IDE / arduino-cli network port)

void Net::setupOta() {
    ArduinoOTA.setHostname(kHost);
    ArduinoOTA.setPassword(d_.settings->apPass);
    ArduinoOTA.onStart([]() { Net* n = gNetSelf; n->d_.sup->setHoldIntake(true); n->server_.stop(); n->otaPct_.store(0); });
    ArduinoOTA.onProgress([](unsigned int done, unsigned int total) { if (total) gNetSelf->otaPct_.store(static_cast<int>(done * 100 / total)); });
    ArduinoOTA.onEnd([]() { gNetSelf->otaPct_.store(100); });
    ArduinoOTA.onError([](ota_error_t e) { Net* n = gNetSelf; n->otaPct_.store(-2 - static_cast<int>(e)); n->d_.sup->setHoldIntake(false); n->server_.begin(); });
    ArduinoOTA.begin();
}

// loop() prints OTA progress so the message goes through the console (Serial + log ring) from one task
void Net::otaProgressToLog() {
    int p = otaPct_.load();
    if (p == otaLoggedPct_) return;
    if (p >= 0 && (p == 0 || p == 100 || p / 10 != otaLoggedPct_ / 10)) d_.console->printf("OTA %d%%%s", p, p == 100 ? " done, restarting" : p == 0 ? " (motors held)" : "");
    else if (p < -1) d_.console->printf("OTA FAILED (%d); the old firmware is intact", -(p + 2));
    otaLoggedPct_ = p;
}

// ---------------------------------------------------------------- HTTP helpers

void Net::sendJson(int code, const char* body) {
    server_.sendHeader("Cache-Control", "no-store");
    server_.sendHeader("Access-Control-Allow-Origin", "*");
    server_.send(code, "application/json", body);
}
void Net::ok(const char* msg) {
    char b[160]; JsonWriter j(b, sizeof b); j.beginObject(); j.kv("ok", true); if (msg) j.kv("msg", msg); j.endObject(); sendJson(200, b);
}
void Net::fail(int code, const char* err, const char* msg) {
    char b[200]; JsonWriter j(b, sizeof b); j.beginObject(); j.kv("ok", false); j.kv("err", err); if (msg) j.kv("msg", msg); j.endObject(); sendJson(code, b);
}

void Net::registerRoutes() {
    // the page: gzipped PROGMEM, revalidated by ETag so an OTA never leaves a stale page in a phone
    for (unsigned int i = 0; i < WEB_ASSET_COUNT; ++i) {
        const WebAsset& a = WEB_ASSETS[i];
        auto serve = [this, &a]() {
            String etag = String("\"" WEB_ASSETS_BUILD "-") + a.len + "\"";
            server_.sendHeader("ETag", etag); server_.sendHeader("Cache-Control", "no-cache");
            if (server_.header("If-None-Match") == etag) { server_.send(304, a.mime, ""); return; }
            server_.sendHeader("Content-Encoding", "gzip");
            server_.send_P(200, a.mime, reinterpret_cast<const char*>(a.data), a.len);
        };
        server_.on(a.path, HTTP_GET, serve);
        if (std::strcmp(a.path, "/") == 0) server_.on("/index.html", HTTP_GET, serve);
    }
    server_.on("/favicon.ico", HTTP_GET, [this]() { server_.send(204, "text/plain", ""); });
    server_.on("/api/state", HTTP_GET, [this]() { handleState(); });
    server_.on("/api/cmd", HTTP_POST, [this]() { handleCmd(); });
    server_.on("/api/config", HTTP_GET, [this]() { handleConfigGet(); });
    server_.on("/api/config", HTTP_POST, [this]() { handleConfigPost(); });
    server_.on("/api/log", HTTP_GET, [this]() { handleLog(); });
    // E-STOP: no auth, no rate limit, any method; it latches before this handler returns
    auto estop = [this]() { d_.estop->trip(EstopLatch::Source::Web, millis()); ok("E-STOP latched"); };
    server_.on("/api/estop", HTTP_POST, estop);
    server_.on("/api/estop", HTTP_GET, estop);
    server_.on("/api/estop/reset", HTTP_POST, [this]() { bool was = d_.estop->release(millis()); ok(was ? "E-STOP released" : "E-STOP was not latched"); });
    server_.on("/api/stop", HTTP_POST, [this]() { if (!d_.lines->push("!STOP", static_cast<uint8_t>(ReqSource::Web))) fail(503, "busy"); else ok("Stopping"); });
    server_.on("/api/restart", HTTP_POST, [this]() { if (!d_.lines->push("#PRESTART", static_cast<uint8_t>(ReqSource::Web))) fail(503, "busy"); else ok("Restarting"); });
    server_.on("/update", HTTP_POST, [this]() { handleUpdateDone(); }, [this]() { handleUpdateUpload(); });
    server_.onNotFound([this]() {
        String uri = server_.uri();
        if (uri.startsWith("/api/")) { fail(404, "unknown", "no such endpoint"); return; }
        server_.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);   // captive portal probes land on the page
        server_.send(302, "text/plain", "");
    });
}

void Net::handleState() {
    Snapshot s; if (!d_.pub->read(s)) { fail(503, "busy"); return; }
    JsonWriter j(json_, sizeof json_);
    j.beginObject();
    j.kv("fw", UPPITY_FW_VERSION); j.kv("up", static_cast<unsigned long>(millis() / 1000));
    j.kv("state", stateName(static_cast<int>(s.state)));
    j.key("lifter"); j.beginObject(); j.kv("ticks", static_cast<long>(s.lifterTicks)); j.kv("pct", static_cast<int>(s.lifterPct)); j.kv("travel", static_cast<long>(s.lifterTravel)); j.kv("conf", lifterConfName(static_cast<int>(s.lifterConf))); j.endObject();
    j.key("rotary"); j.beginObject(); j.kv("ticks", static_cast<long>(s.rotaryTicks)); j.kv("deg", static_cast<int>(s.rotaryDeg)); j.kv("conf", homeConfName(static_cast<int>(s.homeConf))); j.kvf("tpr", s.ticksPerRev, 1); j.endObject();
    j.key("limits"); j.beginObject(); j.kv("top", s.topLimit); j.kv("bot", s.bottomLimit); j.kv("home", s.homeSwitch); j.endObject();
    j.kv("enabled", s.enabled); j.kv("estop", s.estop); j.kv("estopSource", EstopLatch::sourceName(static_cast<EstopLatch::Source>(s.estopSource)));
    j.kv("faults", static_cast<unsigned long>(s.faults));
    j.kv("job", jobName(static_cast<int>(s.activeJob))); j.kv("jobId", static_cast<unsigned long>(s.activeJobId));
    j.key("last"); j.beginObject(); j.kv("job", jobName(static_cast<int>(s.lastJob))); j.kv("result", resultName(static_cast<int>(s.lastResult))); j.kv("reason", reasonName(s.lastReason)); j.kv("id", static_cast<unsigned long>(s.lastProgramId)); j.endObject();
    j.kv("override", static_cast<unsigned long>(s.overrideRemainingMs / 1000));
    j.kv("commissioned", s.commissioned);
    j.key("tick"); j.beginObject(); j.kv("maxUs", static_cast<unsigned long>(s.cycleUsMax)); j.kv("late", static_cast<unsigned long>(s.lateTicks)); j.endObject();
    d_.console->writeStateJson(j);
    j.kv("log", static_cast<unsigned long>(d_.log->total()));
    j.key("wifi"); writeWifiJson(j);
    j.endObject();
    if (j.overflowed()) { fail(500, "overflow"); return; }
    sendJson(200, json_);
}

void Net::writeWifiJson(JsonWriter& j) {
    j.beginObject();
    j.kv("ap", apSsid_); j.kv("clients", static_cast<int>(apClients_.load()));
    uint8_t st = staState_.load();
    j.kv("sta", d_.settings->staSsid[0] == '\0' ? "off" : st == 2 ? "up" : st == 1 ? "joining" : "down");
    j.kv("ssid", d_.settings->staSsid);
    j.kv("ip", st == 2 ? WiFi.localIP().toString().c_str() : WiFi.softAPIP().toString().c_str());
    j.kv("rssi", st == 2 ? static_cast<int>(WiFi.RSSI()) : 0);
    j.kv("host", kHost);
    j.endObject();
}

void Net::printStatus(TextSink& out) {
    if (!up_) { out.line("WIFI=off"); return; }
    uint8_t st = staState_.load();
    out.printf("WIFI=ap \"%s\" %s clients=%u  home=%s%s%s  http://%s.local", apSsid_, WiFi.softAPIP().toString().c_str(), (unsigned)apClients_.load(),
               d_.settings->staSsid[0] ? d_.settings->staSsid : "(none)", st == 2 ? " up " : st == 1 ? " joining" : d_.settings->staSsid[0] ? " down" : "",
               st == 2 ? WiFi.localIP().toString().c_str() : "", kHost);
}

// ":P..."/"#P..." lines from the page go to loop() like any other serial source
void Net::handleCmd() {
    String line = server_.hasArg("line") ? server_.arg("line") : server_.arg("plain");
    line.trim();
    if (line.length() == 0 || line.length() > 200 || (line[0] != ':' && line[0] != '#')) { fail(400, "invalid", "a command starts with :P or #P"); return; }
    if (!d_.lines->push(line.c_str(), static_cast<uint8_t>(ReqSource::Web))) { fail(503, "busy", "command queue full"); return; }
    ok();
}

void Net::handleConfigGet() {
    const Settings& s = *d_.settings;
    JsonWriter j(json_, sizeof json_);
    j.beginObject();
    for (int i = 0; i < kSettingKeyCount; ++i) j.kv(kSettingKeys[i].name, static_cast<long>(kSettingKeys[i].get(s)));
    for (int i = 0; i < kStringKeyCount; ++i) { const StringKey& k = kStringKeys[i]; if (k.secret) j.kv(k.name, stringField(s, k)[0] ? "(set)" : ""); else j.kv(k.name, stringField(s, k)); }
    j.kv("apSsid", apSsid_);
    const Calibration& c = d_.console->calibration();
    j.key("cal"); j.beginObject(); j.kv("valid", c.valid != 0); j.kv("travel", static_cast<long>(c.travel)); j.kv("minUp", static_cast<long>(c.minUp)); j.kv("minDown", static_cast<long>(c.minDown)); j.kv("motor", static_cast<long>(c.motor)); j.endObject();
    j.endObject();
    if (j.overflowed()) { fail(500, "overflow"); return; }
    sendJson(200, json_);
}

void Net::handleConfigPost() {
    String key = server_.arg("key"), value = server_.arg("value");
    key.trim(); key.toUpperCase();
    if (key.length() == 0 || key.length() > 12) { fail(400, "invalid", "key"); return; }
    for (unsigned i = 0; i < key.length(); ++i) if (key[i] < 'A' || key[i] > 'Z') { fail(400, "invalid", "key"); return; }
    String line = "#P" + key + "=" + value;
    if (line.length() > 200 || !d_.lines->push(line.c_str(), static_cast<uint8_t>(ReqSource::Web))) { fail(503, "busy"); return; }
    ok();
}

// GET /api/log?since=<offset>|tail  -> {"from":n,"next":m,"text":"..."}
void Net::handleLog() {
    String since = server_.arg("since");
    uint32_t total = d_.log->total();
    uint32_t from = since == "tail" || since.length() == 0 ? (total > 2048 ? total - 2048 : 0) : static_cast<uint32_t>(strtoul(since.c_str(), nullptr, 10));
    uint32_t actual = from;
    size_t n = d_.log->read(from, logBuf_, 3000, &actual);        // leaves the JSON escaper headroom in logJson_, so a chunk can never overflow and strand the page
    logBuf_[n] = '\0';
    JsonWriter j(logJson_, sizeof logJson_);
    j.beginObject(); j.kv("from", static_cast<unsigned long>(actual)); j.kv("next", static_cast<unsigned long>(actual + n)); j.kv("text", logBuf_); j.endObject();
    if (j.overflowed()) { fail(500, "overflow"); return; }
    sendJson(200, logJson_);
}

// ---------------------------------------------------------------- web OTA (POST /update, app .bin)

void Net::handleUpdateUpload() {
    HTTPUpload& up = server_.upload();
    if (up.status == UPLOAD_FILE_START) {
        otaRefused_ = !disarmed(); otaFailed_ = false;
        if (otaRefused_) return;
        d_.sup->setHoldIntake(true);
        otaPct_.store(0);
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { otaFailed_ = true; }
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (otaRefused_ || otaFailed_) return;
        if (Update.write(up.buf, up.currentSize) != up.currentSize) otaFailed_ = true;
        else { size_t done = Update.progress(), total = Update.size(); if (total) otaPct_.store(static_cast<int>(done * 100 / total)); }
    } else if (up.status == UPLOAD_FILE_END) {
        if (otaRefused_ || otaFailed_) { if (!otaRefused_) Update.abort(); return; }
        if (!Update.end(true)) otaFailed_ = true; else otaPct_.store(100);
    } else if (up.status == UPLOAD_FILE_ABORTED) {
        Update.abort(); otaFailed_ = true;
    }
}

void Net::handleUpdateDone() {
    if (otaRefused_) { fail(409, "armed", "update refused: a motor is enabled or a job is running"); return; }
    if (otaFailed_ || Update.hasError()) { d_.sup->setHoldIntake(false); otaPct_.store(-2); fail(500, "update", "update failed; the old firmware is intact"); return; }
    ok("Update written, restarting");
    delay(300);
    ESP.restart();
}

} // namespace uppity
#endif
