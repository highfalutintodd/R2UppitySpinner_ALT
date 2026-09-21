// Phase 3: the JSON writer, the log ring, the web line queue, WiFi settings.
#include "uppity_test.h"
#include "../../src/core/Json.h"
#include "../../src/core/TextRing.h"
#include "../../src/core/LineQueue.h"
#include "../../src/core/Settings.h"
#include <string>
using namespace uppity;

TEST(json_writer_escapes_and_nests) {
    char buf[256]; JsonWriter j(buf, sizeof buf);
    j.beginObject();
    j.kv("s", "a\"b\\c\nd\x01"); j.kv("n", 42L); j.kv("b", true); j.kvf("f", 1185.55, 1);
    j.key("arr"); j.beginArray(); j.num(1); j.str("x"); j.null(); j.endArray();
    j.key("o"); j.beginObject(); j.kv("k", 0); j.endObject();
    j.endObject();
    CHECK(!j.overflowed());
    CHECK(std::string(buf) == "{\"s\":\"a\\\"b\\\\c\\nd\\u0001\",\"n\":42,\"b\":true,\"f\":1185.5,\"arr\":[1,\"x\",null],\"o\":{\"k\":0}}");
    char tiny[8]; JsonWriter t(tiny, sizeof tiny); t.beginObject(); t.kv("key", "value"); t.endObject();
    CHECK(t.overflowed()); CHECK(std::strlen(tiny) < sizeof tiny);
}

TEST(text_ring_offsets_wrap_and_report_gaps) {
    NoLock lock; TextRing<64> r(lock);
    r.line("hello");                                    // 6 bytes
    CHECK_EQ(r.total(), 6u);
    char out[64]; uint32_t from = 99;
    CHECK_EQ(r.read(0, out, sizeof out, &from), 6u); CHECK_EQ(from, 0u); CHECK(std::string(out, 6) == "hello\n");
    CHECK_EQ(r.read(6, out, sizeof out, &from), 0u);     // nothing new
    for (int i = 0; i < 20; ++i) r.line("0123456789");   // 220 bytes: wraps three times
    CHECK_EQ(r.total(), 226u); CHECK_EQ(r.oldest(), 162u);
    CHECK_EQ(r.read(0, out, sizeof out, &from), 64u); CHECK_EQ(from, 162u);   // asked for 0, told where it really started
    CHECK(out[63] == '\n');
    CHECK_EQ(r.read(200, out, 10, &from), 10u); CHECK_EQ(from, 200u);
    CHECK_EQ(r.read(999, out, sizeof out, &from), 0u); CHECK_EQ(from, 226u);   // past the end clamps
}

TEST(line_queue_round_trip_and_full) {
    NoLock lock; LineQueue<2, 16> q(lock);
    CHECK(q.push(":PP100", 7)); CHECK(q.push("#PSTATE")); CHECK(!q.push("dropped")); CHECK_EQ(q.dropped(), 1u);
    char out[16]; uint8_t tag = 0;
    CHECK(q.pop(out, sizeof out, &tag)); CHECK(std::string(out) == ":PP100"); CHECK_EQ(tag, 7);
    CHECK(q.pop(out, sizeof out)); CHECK(std::string(out) == "#PSTATE"); CHECK(!q.pop(out, sizeof out));
    CHECK(q.push("a very long line that will be cut")); CHECK(q.pop(out, sizeof out)); CHECK_EQ(std::strlen(out), 15u);
}

TEST(settings_schema3_wifi_fields_and_migration) {
    Settings s;
    CHECK(std::string(s.apPass) == "Astromech"); CHECK_EQ(s.wifiEnabled, 1); CHECK_EQ(s.apChannel, 1); CHECK_EQ(s.theme, 0); CHECK(s.staSsid[0] == '\0');
    // a schema-2 blob (80 bytes) migrates: the motion prefix survives, the WiFi tail takes defaults
    Settings old; old.schema = 2; old.travelTicks = 1150; old.safeSpinTicks = 904; old.commissioned = 1; old.randomLight = 7;
    uint8_t blob[sizeof(Settings)]; std::memcpy(blob, &old, sizeof old);
    Settings m; CHECK_EQ(migrateSettings(m, blob, kSettingsSchema2Size), 2);
    CHECK_EQ(m.schema, kSettingsSchema); CHECK_EQ(m.travelTicks, 1150); CHECK_EQ(m.commissioned, 1); CHECK_EQ(m.randomLight, 7);
    CHECK(std::string(m.apPass) == "Astromech"); CHECK_EQ(m.wifiEnabled, 1); CHECK(m.staSsid[0] == '\0');
    // string keys
    const StringKey* k = findStringKey("SSID", 4); CHECK(k != nullptr); CHECK(!k->secret);
    setStringField(s, *k, "My Net 5G"); CHECK(std::string(s.staSsid) == "My Net 5G");
    k = findStringKey("APPASS", 6); CHECK(k->secret); setStringField(s, *k, "short"); sanitize(s); CHECK(std::string(s.apPass) == "Astromech");   // WPA2 floor
    setStringField(s, *k, "mydroidpass"); sanitize(s); CHECK(std::string(s.apPass) == "mydroidpass");
    k = findStringKey("PASS", 4); setStringField(s, *k, std::string(100, 'x').c_str()); sanitize(s); CHECK_EQ(std::strlen(s.staPass), 64u);
    CHECK(findStringKey("NOPE", 4) == nullptr);
    // a full round trip keeps the strings
    std::memcpy(blob, &s, sizeof s); Settings back; CHECK_EQ(migrateSettings(back, blob, sizeof blob), kSettingsSchema); CHECK(std::string(back.staSsid) == "My Net 5G"); CHECK(std::string(back.apPass) == "mydroidpass");
    // int keys
    const SettingKey* ik = findSettingKey("APCHANNEL", 9); CHECK(ik->set(s, 6)); CHECK(!ik->set(s, 14)); CHECK_EQ(s.apChannel, 6);
    ik = findSettingKey("THEME", 5); CHECK(ik->set(s, 1)); CHECK(!ik->set(s, 2));
}
