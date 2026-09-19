# Ecosystem facts: Sabé, Roam-A-Dome ALT, DroidNet, Command Library, WCBClient

Generated 2026-09-07 from local clones and upstream repos. Sabé's repo is private; paths cited are local. Versions and library numbers are as of that date.


# Sibling-project research for the new ESP32 periscope firmware

**Method note.** Almost everything below is verified from **local clones**, which turned out to be far richer than the public GitHub surface. Two upstream repos (`travisccook/Droidnet-Command-Library`, `travisccook/DroidNetSignalBooster-releases`) were cross-checked over the web. `https://github.com/highfalutintodd/sabe` **returns HTTP 404 unauthenticated** (`https://api.github.com/repos/highfalutintodd/Sabe`) and does not appear in `https://api.github.com/users/highfalutintodd/repos` — the repo is private, so the local clone is the only source of truth for section A.

---

## Step 1 — Local clones found

| Project | Local path | Git remote / version |
|---|---|---|
| **Sabé** | `/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Control/Amidala Claude Project/Sabe` | `https://github.com/highfalutintodd/Sabe.git` (private). HEAD `e08ca62`, tag `flashed-0.41.1-13-ge08ca62`, fw `0.45.0-logring` |
| **Roam-A-Dome ALT** | `.../PROJECTS/R2-D2 Astromech Droid/Roam-A-Dome/Roam-A-Dome_ALT` | `https://github.com/highfalutintodd/Roam-A-Dome_ALT.git` (public). HEAD `62706c8`, tag `v2.0.0-23-g62706c8`, fw `2.0.0` |
| **DroidNet Signal Booster** (full Pi source) | `.../Amidala Claude Project/DroidNet/droidnet-update-2.2.2` | unpacked update payload, `VERSION` = `2.2.2` |
| DroidNet update archives + Pi images + field logs | `.../PROJECTS/R2-D2 Astromech Droid/DroidNet/Signal Booster/{Updates,Pi Images,Logs}` | zips 1.1 → 2.x |
| DroidNet Mac Maestro | `.../DroidNet/DroidnetMaestro` | `https://github.com/travisccook/DroidNet-MacMaestro.git`, `v1.0.0-4-gb040848` |
| **DroidNet Command Library** (vendored, MPL-2.0) | `.../Sabe/webui/vendor/dcl/` | `libraryVersion` **4.0.0** + Todd's own `sabe-extras.json` |
| DCL (bundled with Signal Booster) | `.../droidnet-update-2.2.2/web/data/command-library/` | `libraryVersion` **2.1.0** |
| **WCB_Client 1.13.0** (vendored) | `.../Roam-A-Dome_ALT/RadFirmware/libs/WCB_Client` | pinned tag 1.13.0 |
| **WCB_Client 1.9.10** (installed) | `~/Documents/Arduino/libraries/WCB_Client` | the version Sabé builds against |
| **WCB firmware + 36 KB Readme** | `.../PROJECTS/R2-D2 Astromech Droid/WCB/Sketches/Legacy/Upgrade - Nov 2025/Wireless_Communication_Board-WCB-main/` | Greg Hulette's board firmware + PCB |
| WCB HCR port-mirror variant | `.../WCB/Sketches/Wireless_Communication_Board-WCB-HCR_Port_Mirror/` | |
| AmidalaFirmware (GPL ancestor) | `.../Amidala Claude Project/AmidalaFirmware` | |
| Sabé PCB gerbers/pinouts | `.../Amidala Claude Project/PCBs/{Gerbers,Pinouts}/Sabe_V1.4_271448RJUN26 FINAL` | |

`~/Documents/Arduino/libraries/` contains: `Adafruit_BusIO`, `Adafruit_NeoPixel`, `Adafruit_PWM_Servo_Driver_Library`, `Adafruit_RGB_LCD_Shield_Library`, `EspSoftwareSerial`, `LedControl`, `PCF8574_library`, `PololuMaestro`, `Reeltwo`, `SlowServoPCA9685`, `USBSabertooth`, **`WCB_Client`**, **`WcbCmd`**.

No local clone of `DroidNetSignalBooster-releases` or `Droidnet-Command-Library` as git repos — but the actual Signal Booster **source** and the DCL **content** are both present as above.

**Current periscope project for contrast:** `.../Arduino/Claude Code/R2UppitySpinnerV3/` is a single **270 KB `R2UppitySpinnerV3.ino`** + **145 KB `WebPages.h`** + `menus/`, `pin-map.h`, `platformio.ini`, and a stale copy of `r2uppityspinner-alt.json`.

---

# A. Sabé — the architectural template

## A1. Repo layout and module ownership

Root: `.../Amidala Claude Project/Sabe/`

```
SabeFirmware/   ESP32-S3 real-time firmware (multi-file Arduino sketch, 46 files)
C6Firmware/     XIAO ESP32-C6 co-processor: ALL WiFi + web server + HTTP↔UART relay
webui/          vanilla HTML/CSS/JS, no build step, no framework + Python dev server
webui/vendor/dcl/  Travis Cook's DroidNet Command Library, vendored (MPL-2.0)
test/           host-side C++ tests — `bash test/run-tests.sh`, no droid needed
docs/           49 files: architecture, protocols, session records, design notes
hardware/       PCB bring-up, flashing procedure, generated board diagrams
Bugs/           field-test captures, one folder per date
spikes/         throwaway de-risking experiments (kept for the record)
BACKLOG.md      92 KB — "what's next", the entry point for a session
CLAUDE.md       project rules + architecture invariants
DEPENDENCIES.md pinned toolchain
```

**It is Arduino, not PlatformIO** — and deliberately so. Build settings are pinned in per-sketch `sketch.yaml` profiles:

- `SabeFirmware/sketch.yaml` → profile `s3`: `esp32:esp32:esp32s3:PartitionScheme=default_8MB,FlashSize=8M,CDCOnBoot=cdc,UploadSpeed=115200`, platform `esp32:esp32 (3.3.4)`, library `WCB_Client (1.9.10)`
- `C6Firmware/sketch.yaml` → profiles `c6` (`XIAO_ESP32C6:PartitionScheme=default,FlashSize=4M,...`) and `c3` (`min_spiffs`)
- Build: `arduino-cli compile --profile s3 SabeFirmware`

The `sketch.yaml` header comments explain *why*: Arduino IDE 2.x re-identifies the board on a port change and silently drops Tools options, producing `{build.partitions}.csv: No such file` or `Maximum is 1310720 bytes`. **This is a pattern worth copying verbatim.**

### Module map (from `docs/architecture.md`, current; `SabeFirmware/README.md` is explicitly marked a stale historical record)

| Module | Context | Owns |
|---|---|---|
| `SabeControl.h/.cpp` | control task | The 50 Hz cycle: watchdog pet, `Safety_update`, `evaluateSignalFailsafe`, `driveCycle`, `domeCycle` |
| `SabeControlCore.h/.cpp` | pure | Testable control math extracted from the task |
| `SabeSafety.h/.cpp` | both | State machine BOOT/SAFE_IDLE/OPERATIONAL/FAILSAFE_WARN/EMERGENCY_STOP, one-way latch, `Safety_speedScale()`, `Safety_isDriveAllowed()` |
| `SabeXBee.h/.cpp` | loopTask writes / control reads | ZigBee API mode-2 parser → snapshot under portMUX; Remote-AT `%V` battery poll |
| `SabeDrive` / `SabeDome` | control task | Dumb LEDC PWM actuators. `ledcWrite` appears nowhere else in the firmware |
| `SabeButtons` / `SabeButtonEdge` | loopTask / pure | Short/long/double/triple press edge detection; LIVE/STANDBY posture; arming chord |
| `SabeActions` | loopTask | Persisted action table; `!ESTOP` / `!STANDBY` token expansion |
| `SabeMesh` | loopTask | WCBClient wrapper, device 20; splits `^`-chains; **delivery only** |
| `SabeMeshRoute.h/.cpp` | **pure** | The routing *decision* as a pure function — no Arduino, no WCBClient, no state, host-tested (~55 assertions) |
| `SabeDroidNet` | loopTask **and** ESP-NOW RX callback | The `&` protocol both ways |
| `SabeSound` | loopTask | H-CR volume/musing state Sabé asserts rather than inherits |
| `SabeWdp` | loopTask | Polls WCBClient's WDP neighbor table into a snapshot |
| `SabeVictron` | loopTask | `?VSTATS` request/response over UART0 to a WCB S-port |
| `SabeConfig` / `SabeConfigLegacy` | loopTask | NVS blob, schema v16, append-only with real migrations |
| `SabeStealthImport` | loopTask | Stealth `config.txt` line parser |
| `SabeConsole` | loopTask | Serial CLI; `Console_setConfig()` is the ONE validation path |
| `SabeBridge` | loopTask | UART2 line-request server for the C6 |
| `SabeLog` | any | Console tee → 16 KB RAM ring, read out over the bridge |
| `SabeBoard.h` | — | **Every pin, per board variant** (`SABE_BOARD_PCB_V1` vs `SABE_BOARD_DEVBOARD`) |
| `SabeVersion.h` | — | One `#define SABE_FW_VERSION "0.45.0-logring"` |

**The extraction pattern to copy:** `SabeMeshRoute`, `SabeControlCore`, `SabeButtonEdge` are *pure* headers/cpp — no `Arduino.h`, no library handles, no state — so the *decision* is host-testable on a laptop and only the *delivery* stays in the Arduino module.

## A2. Library dependencies

**S3 firmware includes, exhaustively** (`grep '#include <'` over all 46 files):

```
Arduino.h, Preferences.h, WCB_Client.h, WiFi.h (only for WiFi.persistent(false)),
esp_task_wdt.h, freertos/FreeRTOS.h, freertos/task.h,
ctype.h math.h stdarg.h stddef.h stdint.h stdio.h stdlib.h string.h strings.h
```

**C6 firmware includes:** `WiFi.h`, `WebServer.h`, `ESPmDNS.h`, `ArduinoOTA.h`, `DNSServer.h`, `Preferences.h`, `esp_random.h`, `SabeC6Assets.h`.

Answering your checklist precisely:

- **ReelTwo: avoided entirely.** `grep -rn "ReelTwo\|Reeltwo\|reeltwo"` over `SabeFirmware/` and `C6Firmware/` returns **nothing**. (`DEPENDENCIES.md` lists Reeltwo 23.5.3 as *installed on the machine*, not linked.)
- **ESPAsyncWebServer: NO.** Uses the core's synchronous `WebServer.h`.
- **ArduinoJson: NO.** JSON is **hand-rolled by string concatenation** on both chips, with a deliberate `jsonStr()` escaper for user text (SSIDs) at `C6Firmware.ino`.
- **Preferences: YES**, both chips. S3 namespace via `NVS_NAMESPACE` in `SabeConfig.cpp`; C6 namespace `"sabec6"`.
- **LittleFS / SPIFFS: NO.** `C6Firmware/README.md`: *"this firmware never uses SPIFFS."* Assets are gzipped PROGMEM.
- **ESP-NOW: yes, but only via WCB_Client**, never `esp_now.h` directly.
- **Update.h: no direct use** — OTA is `ArduinoOTA` on the C6 only.
- **The S3 has no WiFi, ever** (CLAUDE.md). `WiFi.h` appears once, to call `WiFi.persistent(false)`.

## A3. WiFi model (C6, `C6Firmware/C6Firmware.ino`)

```cpp
WiFi.mode(g_staSsid.length() ? WIFI_AP_STA : WIFI_AP);
WiFi.setHostname(MDNS_NAME);
WiFi.softAP(AP_SSID, g_apPass.c_str());
```

- **AP is always up and comes up first** — "the path that cannot be typo'd away". `AP_SSID = "Sabe"`, default password `astromech` (a compiled-in *default*, not a secret; the UI nags while it's unchanged).
- **STA joins in addition**, from an SSID stored in the C6's own NVS (`stassid`/`stapass`). Compile-time `STA_SSID`/`STA_PASS` are only a seed for a fresh board; stored always wins.
- **Retries are hand-rolled, not `setAutoReconnect`**: `WiFi.setAutoReconnect(false)`, backoff 15 s doubling to a 300 s ceiling, and **the scan is suspended entirely while any client is on the AP** (`WiFi.softAPgetStationNum() > 0`) — because in AP_STA every scan disturbs the AP.
- **Credentials are stored, not applied live.** Joining a new network reboots the AP onto a new channel and would drop the browser that asked. `{"ok":true,"reboot":true}`.
- **Captive portal: YES**, `DNSServer` on port 53, wildcard `*` → `WiFi.softAPIP()`, **but only while STA is disconnected** ("a wildcard DNS responder belongs on an island, not on your house network").
- **mDNS: `sabe.local`.** Critical ordering note in the source: `ArduinoOTA::begin()` internally calls `MDNS.begin(hostname)`, so OTA must be started **before** `MDNS.addService("http","tcp",80)` or the service registration is dropped. `OTA_HOSTNAME` must equal `MDNS_NAME`.
- **Password recovery without hardware:** `wifi-reset` on the S3 console sets a RAM-only flag, reported as `"wifiReset":1` inside the `state` telemetry the C6 already polls every 2 s; the C6 clears both passwords, sends `wifi-reset-done`, and reboots.

## A4. Web server, assets and JSON API

**Assets:** `webui/build-c6-assets.py` gzips every UI file into `C6Firmware/SabeC6Assets.h` as PROGMEM arrays with `{path, data, len, mime}`. Rationale in the file header: *"one-file generated header → the C6 sketch compiles and flashes from the Arduino IDE with no filesystem-uploader plugin, no second upload step, no way for code and assets to get out of sync."* The device build swaps `mock-api.js` → `sabe-api.js` inside `index.html`.

**Caching (a bug they already hit):** assets used to ship `max-age=86400`, so an OTA delivered fresh HTML against a day-old cached `app.js`. Now everything is `Cache-Control: no-cache` + an `ETag` keyed to `SABE_ASSETS_BUILD` (a hash of the uncompressed sources) with a 304 fast path.

**API conventions:**

| Endpoint | Behaviour |
|---|---|
| `GET /api/state` · `/api/config` · `/api/actions` | relayed verbatim to the S3 (`state`/`config`/`actions`) |
| `GET /api/ping` | C6 vitals wrapped around the S3's `ping` |
| `POST /api/bridge` | body = one bridge line (≤ 220 chars), relayed verbatim |
| `POST /api/login` | body = password → `{"ok":true,"token":"<32 hex>"}` |
| `GET`/`POST /api/wifi` | answered by the C6 itself; the S3 never sees credentials |
| `onNotFound` | 302 → `/` (hash-routed SPA) |

- **Polling, not WebSocket/SSE.** `sabe-api.js` polls `GET /api/state` at **2 Hz** and diffs client-side; `config`/`actions` are re-pulled after every mutation "so the UI always shows the S3's *accepted* values, never optimistic ones". WebSocket is documented as a v2 candidate in `webui/API-CONTRACT.md`.
- **Auth:** `X-Sabe-Token` header, a **ring of 4 tokens** (four devices stay logged in; a fifth evicts the oldest), RAM-only, dies on reboot. GET routes are open.
- **`estop` and `disable` are exempt from auth by design** — "stopping is never gated on the things that gate starting". `estop-reset` is *not* exempt, because clearing a latch is a starting action.
- **OTA update page: none.** OTA is `ArduinoOTA` over WiFi, driven from the Arduino IDE's Network Ports (`sabe-c6`), password default `astromech`, and it **refuses while the droid is armed**.

## A5. Web UI design system (`webui/sabe.css`, 965 lines)

```css
html { font-size: 20px; }            /* the master text-size knob; everything in rem */
--sans: "Schibsted Grotesk", "Hanken Grotesk", "Helvetica Neue", system-ui, sans-serif;
--mono: "JetBrains Mono", "SF Mono", Menlo, Consolas, monospace;
```

**Light ("paper") is the default**, dark ("slate") via `html[data-theme="dark"]`, persisted in `localStorage['sabe-theme']`:

| Token | paper (light) | slate (dark) |
|---|---|---|
| `--bg` | `#F3EEE2` | `#15171C` |
| `--raise` (cards) | `#FBF9F3` | `#1C1F25` |
| `--raise2` | `#ECE6D8` | `#262A31` |
| `--well` / `--field` | `#EAE3D4` / `#FFFEFA` | `#101216` |
| `--text` | `#211C17` | `#E9E9E6` |
| `--accent` / `--accent-hi` | `#7A2230` / `#93293A` | `#A93246` / `#C7475B` |
| `--ok` / `--warn` / `--bad` | `#38754F` / `#8F6B22` / `#93293A` | `#55A877` / `#D8AC52` / `var(--accent-hi)` |

Burgundy accent; the logo jewel stays `#7A2230`. Body carries a `radial-gradient` wash.

**Layout:** sticky `header` (dome mark + wordmark + `.descriptor` letterspaced caps + spacer + theme toggle + state pill + E-STOP button) → `.shell { display:flex }` with an **11 rem left `nav`** of uppercase letterspaced links and `main { flex:1; padding:1.2rem; max-width:1100px }`. Pages are `section.page` shown/hidden by hash route, with a `pagein` fade. `h2` has a 2.5 rem accent underline via `::after`. Cards are `.card` in a `.grid` of `repeat(auto-fill, minmax(15rem, 1fr))`, 12 px radius.

**Nav pages:** Dashboard · Drive · Dome · Remotes · Sound & Emotion · WCB Network · Safety · Log · System · About.

**Semantics worth reusing:**
- `.state-pill.{SAFE_IDLE|OPERATIONAL|FAILSAFE_WARN|EMERGENCY_STOP}`; `EMERGENCY_STOP` throbs.
- `.state-pill.NO_LINK` is **dashed amber, deliberately neither green nor red** — "the droid's state is unknown, which is neither 'fine' nor 'stopped', and it must not be mistaken for either."
- `button.estop.failed` turns amber and throbs when the *send* failed — the failure is shown on the button, "because the log is a tab you have to be on".
- Sticky banners: `#estop-banner`, `#armed-banner`, `#auth-banner`, `#batt-banner`, `#learn-banner`, all clearing a JS-measured `--hdr-h`.
- Every error code has a plain-English gloss in `ERR_TEXT` in `webui/app.js` — "a new code needs an entry there or the UI will show the bare token".
- Fonts load from Google when online and **fall back gracefully on the droid's AP**; a to-do notes embedding WOFF2 subsets before shipping.

`python3 webui/dev-server.py` → `http://localhost:8642` runs the full C6 simulation (REST + bridge semantics + refusal codes + wiggling sticks) with no hardware.

## A6. Serial protocols

### The mesh command grammar Sabé speaks (WCB, from Greg's Readme §2, verified against `SabeMeshRoute.h`)

| Form | Meaning |
|---|---|
| `?…` | WCB-local config (`?CONFIG`, `?STOP`, `?EPASS…`, `?SMS4R,S0`, `?BCAST,OUT,S4,ON`, `?SPECIAL,ON`) |
| `;W<x>;S<y><payload>` | send `<payload>` to WCB *x*, out its Serial *y* — e.g. `;W3;S4:PP100` |
| `;S<y><payload>` | out the *local* WCB's Serial *y* |
| `;C<key>` | recall a stored command by key (`;C01`…) |
| `;M<x><yy>` | Maestro *x*, sequence *yy* (0 = all) |
| `;T<ms>,` | timer prefix on a chained part |
| `^` | chain separator |
| `<…>` | H-CR vocalizer native (unicast-only syntax) |
| `~RT…` | logic engine |
| `:DP…` / `#DP…` | Roam-A-Dome |
| `:P…` / `#P…` | **periscope (R2 Uppity Spinner)** |

**Sabé's routing priority** (`SabeMeshRoute.h`, `MeshRoute_decide()`): `;W<n>[,]<rest>` → WCB n, prefix stripped (explicit, outranks all) · `<…>` → H-CR host · `;C##`/`;SEQkey` → sequence host · `;M<X>YY` → whichever board **advertised Maestro X over WDP**, falling back to WCB X by wiring convention · else **broadcast**. `;T` is refused ("Sabé is not a sequencer").

**Broadcast length ceiling — a hard limit for any periscope command a UI composes:**
```
MESH_BCAST_FIELD_CHARS 199 ; MESH_BCAST_CRC_CHARS 12
→ 187 chars with checksum on (the fleet runs ?ETM,CHKSM,ON), 199 without
```
Broadcasts **never fragment**; over the ceiling WCB_Client returns false rather than truncating. Unicasts auto-fragment to ~2.8 KB.

### The `&` DroidNet namespace (`docs/sabe-mesh-protocol.md`)

Design rule 1: **prefix `&` for anything in this family** — unused by the rest of the ecosystem, so non-participants ignore it. Comma-separated ASCII, uppercase keywords, no spaces.

**Heartbeat — exact format**, from `SabeDroidNet.cpp:106`:
```c
snprintf(buf, sizeof buf, "&SABE,HB,%s,%lu,%s,%ld,%ld", ...)
```
```
&SABE,HB,<fwver>,<uptimeS>,<state>,<driveAgeMs>,<domeAgeMs>
```
Real lines captured from the wire in `Bugs/`:
```
&SABE,HB,0.37.0-doubletap,13,SAFE_IDLE,-1,-1
&SABE,HB,0.37.0-doubletap,1113,OPERATIONAL,41,47
```
`-1` = no link. **Rate: default 10 s, but operator-settable 5–300 s** since schema v15 / fw 0.41.0 via `set hbperiod` — the doc explicitly warns *"do NOT size a liveness timeout from the default alone"*. Every heartbeat carries the firmware version, and the current period is in the `config` dump. (Todd currently runs it at 30 s to cut mesh congestion — `docs/hcr-s5-truncation-handover-2026-08-19.md`.)

**Other outbound (event-driven, sent immediately on transition):**
```
&SABE,STATE,<state>,<scale>          e.g. &SABE,STATE,OPERATIONAL,1.00
&SABE,ESTOP,<reason>                 signal-lost|console|web|mesh|hw-switch
&SABE,ACK,<cmd>   /   &SABE,NAK,<cmd>,<reason>
&SABE,HELLO,<fw>,<protover>
&SABE,BATT,<drive|dome>,<mV>,<OK|LOW|CRIT>    on level change only
```
Multi-part command names join with `.` in receipts so CSV fields stay clean: `&SABE,ACK,ESTOP.RESET`.

**Inbound verbs accepted:** `&ESTOP`, `&ESTOP,RESET`, `&DISABLE`, `&STATUS`, `&HELLO`, `&SAY,<wav#>`. `&ENABLE` was **removed in 0.36.0** and always answers `NAK,ENABLE,removed` — nothing on the mesh may ever arm the droid.

**A discipline the periscope must inherit** (fw 0.39.3+, from `docs/congestion-remediation-2026-08-17.md`): **anything outside the verb list gets no answer at all.** NAK-broadcasting a reply to every foreign `&RAD,POS,…` was measurably the largest single source of mesh junk on this droid. New verbs go into `isSabeVerb()` in `SabeDroidNet.cpp` or they are silently ignored.

### Multi-source serial handling

Four hardware UARTs, all spoken for on the S3: console = **native USB-CDC** (so `USB CDC On Boot` must stay Enabled — with it off, `Serial` maps to GPIO43/44 where the C6 bridge lives and the two fight); XBee = UART1; C6 bridge = UART2; Victron/WCB1-S4 = **UART0 remapped to GPIO11/12** (the boot ROM chatters on 43/44 before `setup()` remaps, so the WCB never sees it).

**Console↔bridge unification:** both surfaces route config edits through `Console_setConfig()` — invariant 8: *"The bridge trusts nothing."* All rules (stop always honored, arming gated, latch freezes shows, rate limits, validation) are enforced on the **S3**, never in the C6 or the browser.

**Bridge wire format** (`docs/sabe-bridge-protocol.md`), UART 921600 8N1:
```
C6 → S3   <id> <verb> [args...]\n     id = decimal, echoed verbatim
S3 → C6   <id> {json}\n               single line
```
Stale lines skipped by id mismatch. C6 answers HTTP 504 `{"err":"s3-timeout"}` after 500 ms. The C6's receive buffer had to be widened 8192 → 14336 because the old cap **silently truncated and returned partial JSON as if complete** — "the worst kind of failure: one that doesn't say so".

**Rate limits:** mesh verbs ≥ 250 ms apart (validated *before* the budget is stamped, so a malformed request can't burn the next legitimate one); config-writing verbs get a separate token bucket, burst 4 then 1/150 ms, charged only when something was actually committed.

### E-STOP conventions across the ecosystem

| Layer | Convention |
|---|---|
| WCB | `?STOP` — stops timer-driven commands fleet-wide |
| Sabé mesh | `&ESTOP` / `&ESTOP,RESET` / `&DISABLE`; fires `?STOP` onto the mesh when it latches |
| Sabé action table | `!ESTOP` maps a physical button to a **local** e-stop, `!STANDBY` stands the pressing remote down |
| Sabé web | `POST /api/estop` — **never authenticated, never rate-limited** |
| Roam-A-Dome | latches on `?STOP` **or** `&SABE,ESTOP` from any transport |
| Periscope (today) | `:PX`, DCL `safety: "power"` |

Universal rule, stated four separate places in the source: **stopping is never gated on the things that gate starting.** The latch is one-way; reset is deliberate-only and lands in `SAFE_IDLE`, never straight back to `OPERATIONAL`.

## A7. NVS, logging, tasks, safety

**NVS / config (`SabeConfig.h/.cpp`, `SabeConfigLegacy.h`):** one `Preferences`-backed struct blob, `SABE_CONFIG_SCHEMA_VERSION 16`, stamped on save and checked on load. **Append-only** with real migrations (v3→v16 all upgrade in place). Ranges are `#define`d as a single source of truth for both setters and `Config_sanitize()`, which clamps every field on load. A failed/short/unparseable read runs on defaults **without overwriting** what is stored. `Config_save()` skips the write entirely when the blob is unchanged.

To add a field: freeze the current struct in `SabeConfigLegacy.h`, append, bump the version, add a migration branch, extend `test/test_config.cpp`. "A firmware update must never eat a user's learned remotes, action table, or calibration."

**Logging (`SabeLog.h`, fw 0.45.0):** every `.cpp` that prints includes `SabeLog.h` **last**, which `#define Serial SabeLogTee` — a `Stream` subclass tee'ing to the real HWCDC *and* into a 16 KB RAM ring. Read out over the bridge verb `dlog [from]` → `{"from":N,"next":N,"total":N,"dropped":N,"text":"…"}`, polled by the Log tab at ~0.7 Hz. Motivation: the native-USB console **resets the chip when a monitor detaches** (proven 4×); the ring over the bridge cannot.

**Tasks/cores — read this carefully, the docs correct an earlier error:**

| Context | Core | Priority | Runs |
|---|---|---|---|
| `sabe_ctrl` control task | **1** | **10** | watchdog, safety machine, failsafe, all motor writes |
| `loopTask` | **1** | **1** | everything else |
| WiFi task | 0 | IDF | ESP-NOW; WCBClient's RX callback |

The isolation is **priority preemption, not a core split** — arduino-esp32 3.3.4 builds with `CONFIG_ARDUINO_RUNNING_CORE=1`, so `loop()` is on Core 1 too. `docs/architecture.md` is blunt that the old "Core 0/Core 1" claim was wrong "because safety arguments built on the wrong mechanism are worth nothing."

**The consequence with teeth: no flash writes while armed.** A flash program/erase disables the instruction cache and stalls **both cores** (`CONFIG_SPI_FLASH_AUTO_SUSPEND` is unset; zero `IRAM_ATTR` in the firmware) while **LEDC autonomously keeps emitting the last commanded PWM** — the droid keeps moving at the last commanded speed for the whole stall (NVS page-GC erase typically 45–150 ms). Enforced since 0.30.0: every config-writing verb is refused while armed, on both surfaces. `Config_save()` is the only flash-write path and every caller is gated.

**Safety chain, in reaction order:**
0. **Per-remote LIVE/STANDBY posture** — RAM-only, STANDBY at every boot, armed only by a two-button chord held 2 s on that remote
1. **Stale-input neutral hold, 250 ms** — fixed, deliberately NOT configurable
2. **Signal failsafe** 500/2000 ms default (config-clamped 250–2000 / 1000–8000), keyed strictly on the *drive* remote
3. **Hardware task watchdog, 300 ms** — fixed
4. **The Roboteq's own pulse-loss brake** — hardware-confirmed backstop

Config may move the **latch** point, never the motion-stop point. **One chokepoint per actuator:** `axis × Safety_speedScale()` gated by `Safety_isDriveAllowed()`; new derates compose *there*. **No Serial from the control task** — safety logging is deferred through a portMUX ring drained in `loop()`.

## A8. Documented philosophy, versioning, release process

`README.md` design principles, verbatim in spirit:
1. **Safety is non-negotiable** — multiple independent layers; motor commands pass one gated chokepoint; the control task preempts everything by priority.
2. **Don't reinvent the ecosystem — join it.** Sabé participates natively in the WCB mesh and rides DroidNet's existing pipelines rather than rebuilding them.
3. **Easy, visual configuration.** No arcane config syntax; a web UI with the droid's actual stored sequences in the pickers.
4. **Don't burn the boats.** Migration is reversible at every step.

`CLAUDE.md` adds: **"Absolutely no runaway droids"** as the one unbreakable rule; bench-first with a written verification procedure for every step; *"Todd is not a professional developer — explain the reasoning, not just the result"*; **"Say what you actually tested"** ("Host tests pass, not compiled, not on hardware" is a complete and welcome answer).

**Versioning:** `0.MINOR.PATCH-tag` in `SabeVersion.h`, where the tag names the feature (`0.45.0-logring`, `0.44.0-hwestop`, `0.18.0-pcb`). Bump when shipping a logical change. The C6 versions independently (`C6_FW_VERSION "0.6.3-bridgebuf"`). Git tags mark what was actually flashed (`flashed-0.41.1`).

**Commits:** imperative subject; body explains *why* + what was verified; **note explicitly when something is untested on hardware**. Update docs in the same commit. **License GPL-2.0** (clean reimplementation informed by Amidala, GPL v2).

---

# B. Roam-A-Dome ALT

`/Users/toddword/Library/Mobile Documents/com~apple~CloudDocs/PROJECTS/R2-D2 Astromech Droid/Roam-A-Dome/Roam-A-Dome_ALT` — public at `github.com/highfalutintodd/Roam-A-Dome_ALT`, fw `2.0.0`, LGPL-2.1 (same as upstream reeltwo).

**Layout:** `RadFirmware/` (`.ino` + `src/` + `libs/WCB_Client` vendored + `test/native/`), `SensorFirmware/` (the sensor ring's own ESP32), `COMMANDS.md`, `BEHAVIOR.md`, `HANDOFF.md`, `CLAUDE.md`, `tools/capture_config.py`, `docs/reference/roam-a-dome.dcl.json`, `legacy/` (the original firmware kept intact).

**Arduino + `sketch.yaml` profiles, same as Sabé** — `rad-display` (LilyGO T-Display-S3, `PartitionScheme=default_8MB,FlashSize=8M,CDCOnBoot=cdc,PSRAM=opi`) and `rad-compact` (classic ESP32). Core 3.3.4 pinned; **WCB_Client vendored at `libs/WCB_Client` (tag 1.13.0)** because it isn't in the Library Manager index.

**Includes, exhaustively:** `Arduino.h`, `Preferences.h`, `WCB_Client.h`, `freertos/{FreeRTOS,queue}.h`, `esp_random.h`, `esp_heap_caps.h`, `esp_lcd_panel_*.h`, C++ stdlib. **No ReelTwo, no WiFi, no web server** — the WiFi web UI was deliberately dropped (BEHAVIOR.md D10, §4 dropped table), along with the SMQ droid remote, the VT100 joystick, `#DPSETUP`, and the rotary menu system.

**Architecture rules (`CLAUDE.md`) that mirror Sabé:**
- **Pure core vs glue** — `src/*.h` classes used by host tests (`SensorRing`, `MotionController`, `Sequencer`, `CircularMath`, `Dedup`, `Command`/`Parser`, `Settings` struct) are pure C++ with no Arduino outside `#ifdef ARDUINO`. IO/NVS live in Arduino-only files.
- **Definition of done = three commands:** `make -C RadFirmware/test/native test`, then compile *both* profiles.
- **Tuning defaults have one source** — add to `RadSettings` first, then mirror through `applyTuning`, the parser table, `setSetting` (with range check), `dumpConfig`, and `COMMANDS.md` — "all six or none".
- **Append-only settings** — `Settings.h`: `constexpr uint16_t kSettingsVersion = 4`, NVS namespace `"rad"`, grow only at the END, prefix-copy migration. A comment records the failure that motivated the rule: *"field-shifted garbage loaded fine once: old wcbOct2 0x3C read back as WCBEN=60."*
- **Everything RAD emits can echo back over the mesh, so output formats must parse as silent no-ops (`KEY=` forms), never as executable commands or "Invalid" spam.** Directly relevant to a periscope that reports position.

**Command grammar:** `:DP…` = motion, `#DP…` = config, on all three transports (USB console, command serial, WCB mesh) with **identical syntax**. Cross-transport duplicates within `#DPDEDUP` ms (default 750) execute once. **Anything not `#DP`/`:DP` is ignored silently; a recognized-but-malformed command answers `Invalid`.** `#DPCONFIG` dumps every setting as **replayable `#DPKEY=VALUE` lines** (D6).

### Heartbeat / serial conventions shared with Sabé — `src/WcbLink.cpp`

```c
snprintf(buf, sizeof(buf), "&RAD,HB,%s,%lu,%s", RAD_FW_VERSION, now/1000, state);
sClient->broadcast(buf, /*ensured=*/false);            // every 10 s, hardcoded

snprintf(buf, sizeof(buf), "&RAD,POS,%d,%c", deg, mode);
sClient->sendToSpecialPeer(buf, /*ensured=*/false);    // ≤1 Hz, on change only

snprintf(buf, sizeof(buf), "&RAD,FAULT,%s", code);
sClient->sendToSpecialPeer(buf, /*ensured=*/true);     // falls back to broadcast
```
Real captured lines: `&RAD,HB,2.0.0,1000,IDLE`. States include `ESTOP`.

**The `&<NAME>,…` family is therefore an established convention, not a Sabé quirk.** A periscope should broadcast `&PER,HB,<fw>,<uptimeS>,<state>` (or similar) on the same shape. Note the split: **heartbeat broadcasts; position/fault unicast to the special peer (Sabé at 20)** — which is exactly the traffic hygiene Sabé's congestion work asked for.

**Mesh setup (`WcbLink::begin`):**
```cpp
sClient = new WCB_Client(oct2, oct3, password, quantity, deviceId, onCommand, onStatus);
sClient->setMeshChannel(ch);  sClient->setChecksum(cs);
sClient->setIdentity("Roam-A-Dome", fwVersion, nullptr, "dome,seq,pos");
sClient->begin();
sClient->enableSpecialPeer(20);   // Sabé
```
Refuses to start unless `wcbEnabled` **and** a password is set. `RxLine.text[200]` — sized to WCB_Client's `structCommand[200]`; a smaller buffer *silently truncated long stored-sequence commands mid-body*.

**RX callback discipline (Core 0):** *"flag or copy, nothing else — no NVS/flash/motor."* It latches `?STOP` / `&SABE,ESTOP` into a volatile bool, drops anything not starting `:` or `#` ("heartbeats, WDP chatter, other boards' traffic: ignore silently"), and pushes the rest into a 16-deep FreeRTOS queue drained by `loop()`.

**Safety invariants:** e-stop releases only on fresh manual input or a **validated** `:DP` command; `#DPAUTO`/`#DPHOME` are runtime-only and **forced off on load** (D12 — "a droid could power up and immediately start moving its own dome because a mode was enabled days earlier"); automation always requires a valid sensor; learned drive polarity is persisted but **frozen** until an explicit `#DPDIRLEARN` (D14 — a runtime re-learn once inverted the loop into positive feedback and ran the dome away at full speed).

**Release process (differs from Sabé, and is the better model for a public board):** semver `v2.0.0` git tags, GitHub Releases carrying **complete flash images** `rad-display-<version>.bin` / `rad-compact-<version>.bin` (bootloader + partition table + firmware, flashed at `0x0` with esptool, "they also reset stored settings"), plus `tools/capture_config.py --capture` / `--replay` for carrying settings across — and it works against the *original* firmware too, which is how existing users migrate. Mesh passwords are deliberately never included in a capture.

**Open item worth knowing** (BEHAVIOR.md §10): RAD has no way to hear "all clear" after a `?STOP` when the remotes are off; a `#DPRESUME` honored from any transport is the candidate fix. **A new periscope firmware should design its e-stop *release* path up front rather than inherit this gap.**

---

# C. DroidNet Signal Booster

Repo `github.com/travisccook/DroidNetSignalBooster-releases` is a **releases-only** repo — its README covers install/hardware and, verified by fetch, documents **nothing** about board protocols. Latest release there: **v2.1.3, 2026-07-15**. The local unpacked **2.2.2** payload at `.../DroidNet/droidnet-update-2.2.2/` is newer and is where all the following facts come from.

**What it is:** a Raspberry Pi turned into a wireless hub — flash Arduino/ESP32 boards, monitor serial, control Maestros, run the Comlink voice UI and the Cantina Studio sequencer, all from a browser. **Hardware:** Pi Zero 2 W (recommended), Pi 3/3B/3B+, Pi 4. Original Pi Zero W explicitly unsupported.

**Runtime:** Python 3, asyncio, systemd services under `/opt/droidnet`, logs in `/var/log/droidnet`, runtime state in `/var/lib/droidnet`. Web root `web/` with `index.html`, `comlink.html`, `studio.html`, `console.html` and `handlers/` (audio, chirp, comlink, device, droid, firmware, hcr, network, system, tailscale, victron, wcb_backup, wcb, wizard_flash).

### How it talks to boards — **USB serial to a WCB, and nothing else**

There is **no MQTT, no ESP-NOW on the Pi, no HTTP-to-board, no Bluetooth-to-board** (BLE is used only to read the Victron shunt). The architecture is:

```
web/port_handler.py   PortHandler — single owner of one serial port
                      one reader task → EventBus; one writer task ← bounded queue
                      always_on reconnect with backoff to 60 s
scripts/wcb_listener.py            always-on subscriber: parses ?-command responses
scripts/wcb_stream_trigger.py      subscriber: token match → play a Maestro sequence
scripts/victron_request_responder.py  subscriber: token match → write a JSON reply
```

Every consumer **subscribes to `handler.subscribe("data", cb)`** on the shared stream; blocking work is dispatched off the event loop. Baud constants (`config/constants.py`): `BAUD_RATE_STANDARD = 115200`, `BAUD_RATE_MAESTRO = 9600`, `BAUD_RATE_MARCDUINO = 9600`. `MAX_COMMAND_LENGTH = 2048`.

**Board identity is by USB port path, not serial number** (`scripts/usb_manager.py`): `unique_id = f"{vid}:{pid}:port:{usb_port_path}"` — "a WCB is *the device on this USB port*". VID table: `2341` Arduino, `1a86` CH340, `0403` FTDI, `10c4` Silicon Labs, `067b` Prolific. `TIMEOUT_DEVICE_INACTIVE_SECONDS = 300`.

**Network discovery is for the Pi, not for boards** — `scripts/droidnet_discovery.py` claims `droidnet.local` via `avahi-publish`, with a claim/backoff/renumber protocol so multiple boosters coexist.

### What a board must implement to be reachable

**This is the definitive integration contract**, and Travis wrote it up for exactly this purpose. Local copy: `.../Sabe/docs/droidnet_victron_battery_stats.txt` — *"A short guide for anyone writing firmware that needs live battery telemetry from a DroidNet Signal Booster over the WCB serial bus."*

> Your board sends a token. The Signal Booster sends back one line of JSON. That's the whole protocol. **No WiFi, no HTTP, no IP address needed.** Requires DroidNet 2.2.2+.

Owner-side setup (four things, and the fourth is the one that bites):
1. Victron monitoring configured and reading on the Victron tab
2. **Enable serial responder** ticked
3. Board wired to an S-port of the WCB whose USB cable goes to the Pi, with a raw mapping: `?SMS<x>R,S0`
4. `?BCAST,OUT,S<x>,ON` — **run after step 3**, because creating the mapping auto-disables broadcast output on that port. Request rides the raw mapping; **reply rides broadcast.** Get the order wrong and the request is heard and answered and the reply is never written to your wire.

Firmware-side rules:
- Send the token as a line; `\n` or `\r\n` both fine. Default `?VSTATS`, **owner-renameable — make it configurable** (Sabé exposes `set victrontoken`, `SABE_VICTRON_TOKEN_MAX 24`).
- Matching is case-insensitive, anywhere in the line, **whole-word**: `?VSTATSX` and `?VSTATS2` do not match; `?VSTATS_2` does.
- **Read until `'\r'`, not `'\n'`.** The Pi writes LF onto USB but the WCB re-emits with CR on your S-port. *"Waiting for `'\n'` is the single easiest way to make a working setup look dead."*
- **≥ 500 ms between requests** — repeats inside that window are dropped silently. Poll every 5–30 s.
- **Don't assume the next line is your reply.** The bus is shared; read lines until one contains `"t":"vstats"`. The tag exists so you can route it.
- Handle three shapes: fresh, `stale:true` (real numbers, dead BLE link), and `{"ok":false,...,"err":"nodata"}`.

Reply schema (`scripts/victron_request_responder.py::build_payload`): `ok, t, v, i, p, soc, ttg, ah, aux, temp, alarms[], age, stale`. `VICTRON_SERIAL_STALE_SECONDS = 120`, `DEFAULT_DEBOUNCE_S = 0.5`.

**Forward-looking path:** on Greg's OTA branch, `?BCAST,OUT,S0,ON` echoes broadcast traffic (including mesh-originated) onto USB, so the exchange can be plain broadcast with no raw mapping and the board no longer has to be on the Pi-connected WCB. Two catches: **your token must not start with `?` or `;`** (either prefix makes the WCB parse the line itself), and mesh messages cap at 200 bytes. Flagged implemented-but-untested.

### Telemetry the booster consumes from boards

Travis's guidance to Todd, recorded in `.../Sabe/docs/sabe-mesh-protocol.md` §4: *"as much as is feasible transmitted across the WCB network via serial — DroidNet listens constantly to traffic on the WCB network and can respond/parse as needed."* So the tap is passive and format-agnostic: **anything a WCB will forward, DroidNet can see.** That is why Sabé's `&SABE,HB` / `STATE` / `ESTOP` / `BATT` messages need no registration anywhere — they simply appear on the tap. Travis confirmed the `&` prefix is believed clean across DroidNet's parsing (marked provisional).

### How it uses the Command Library

`scripts/command_library_manager.py` is the device-side loader:
- Two layers, first valid wins: **overlay** `DATA_DIR/droidnet-command-library/` (downloaded via "Update Library") → **bundled** `WEB_DIR/data/command-library/`
- Update flow: fetch `releases.json` → compare `latest.libraryVersion` to installed → fetch `latest.url` (the manifest) → fetch every `manifest.boards[].file` **in order** → validate incl. cross-file conflicts → **install atomically**; any failure aborts and keeps the prior catalog
- `DEFAULT_RELEASES_URL = https://raw.githubusercontent.com/travisccook/droidnet-command-library/main/releases.json`
- Guards: `MAX_BOARD_BYTES 2 MB`, `MAX_MANIFEST_BYTES 256 KB`, `MAX_BOARDS 64`, `HTTP_TIMEOUT 30`, `_VALID_KINDS = ("device-native", "wcb-verb")`, and `_SAFE_BOARD_FILE = ^boards/[A-Za-z0-9._-]+\.json\Z` — anchored with `\Z` not `$` so a trailing newline can't smuggle a path

The bundled catalog on this Pi is `libraryVersion 2.1.0` (8 boards) — **older than upstream 4.2.0 and than Sabé's vendored 4.0.0**.

---

# D. DroidNet Command Library

`github.com/travisccook/Droidnet-Command-Library`, **MPL-2.0**. Todd has a fork (`highfalutintodd/Droidnet-Command-Library`, updated 2026-07-06) and vendors it into Sabé at `.../Sabe/webui/vendor/dcl/`.

**Repository layout** (from the upstream README):
```
libraries/manifest.json           catalog entry point + libraryVersion
libraries/boards/<id>.json        one board per file
src/droidnet-command-library.js       engine (encode/parse/match, round-trip)
src/droidnet-command-library-ui.js    inline step composer, drag-reorder
schema/library.schema.json        formal JSON Schema
scripts/validate.js               CI — `npm run validate`
releases.json                     update pointer for host apps
docs/                             integration + authoring guides
```

**`releases.json`** (fetched live):
```json
{ "$schema": "droidnet-command-library/releases/v1",
  "latest": { "libraryVersion": "4.2.0", "schemaVersion": "v1", "releasedAt": "2026-07-22",
              "url": ".../libraries/manifest.json", "notes": "…board catalog 19 -> 21" },
  "libraries": [ { "id": "droidnet-astromech", "name": "DroidNet Astromech Boards", … } ] }
```

**Manifest schema:** `$schema: "droidnet-command-library/catalog/v1"`, `libraryVersion` (semver, "bump it on every catalog change"), `schemaVersion: "v1"`, `generatedFrom`, and `boards: [{ id, file, name, confidence }]`. Board registration is literally two steps: list the filename in the manifest, drop the file in `libraries/boards/`.

**Board file schema** (`schema/library.schema.json`):

| Level | Required | Optional |
|---|---|---|
| root | `components` (≥1) | `$schema`, `libraryVersion` (`^\d+\.\d+\.\d+([.-].*)?$`), `generatedFrom`, `enums` |
| component | `id` (`^[a-z0-9][a-z0-9-]*$`), `name`, `kind` ∈ **`device-native` \| `wcb-verb`**, `commands` (≥1) | **`confidence` ∈ `high` \| `community` \| `low`**, `firmware`, `vendor`, `routing`, `categories[]` |
| command | `id` (`^[A-Za-z0-9][A-Za-z0-9._-]*$`), `name` | `category`, **`safety` ∈ `cosmetic` \| `movement` \| `power` \| `config`**, `encoder` (default `template`), `template` (required when encoder is `template`), `params[]`, `supportsDuration`, `examples[]`, `commentLabel` |
| param | `name` (`^[A-Za-z][A-Za-z0-9]*$`) | `label`, `enum` (names an entry in root `enums`), `type: "int"`, `min`, `max`, `default`, `required`, `pad`, `pattern` |

`routing` on the uppity board: `{ "class": "broadcast", "nativeWrapper": "none", "durationSuffix": { "supported": false } }`. `sabe-extras` uses `{ "class": "wcb-verb" }`.

**Upstream catalog at 4.2.0 — 21 boards:** flthy-hps, ia-magic-panel, pd-magic-panel, rseries-logic, wcb-hcr, wcb-mp3, maestro, maestro-native, psi-pro, hcr-native, chirp, **r2uppityspinner-alt**, roam-a-dome, astropixels-{plus,sound,psi,logics,holo,servo}, wcb-native, wcb-wled.

**Todd's boards in the catalog:**
- **`r2uppityspinner-alt`** — upstream, `confidence: "community"`, `generatedFrom: "R2UppitySpinnerV3 ALT fw 3.5.3 (highfalutintodd fork of Mimir Reynisson's original)"`
- **`roam-a-dome`** — upstream, `confidence: "high"`, `generatedFrom: "reeltwo DomeControlFirmware (Roam-A-Dome) — :DP motion + #DP config (consolidated)"`. Note this describes the *upstream* firmware; RAD ALT keeps the grammar fully compatible and carries its own conformance copy at `Roam-A-Dome_ALT/docs/reference/roam-a-dome.dcl.json`
- **`sabe-extras`** — **not upstream**, Sabé-local only. One component `wcb-seq` / "WCB Stored Sequences", `kind: "wcb-verb"`, one command `seq.recall` template `{seq}` with an enum `sabe.seq` of ~40 human-labelled entries — Todd's real `;C01`–`;C21` sequences plus `:PS`/`:PL`/`:PP` periscope shows and `<CA10xx>` music. The vendored README calls it "a candidate to upstream once the shared-catalog conversation with Travis lands"

### `r2uppityspinner-alt.json` — every command it declares

41 commands, fw v3.5.3, `kind: device-native`, `confidence: community`. Categories: Lifter, Rotary, Sequences, Lighting, Power, Setup, Config.

| id | template | safety | category |
|---|---|---|---|
| `uppity.playseq` | `:PS{seq}` (0–100) | movement | Sequences |
| `uppity.lifter.seek` | `:PP{position}` (0–100) | movement | Lifter |
| `uppity.lifter.seek.speed` | `:PP{position},{speed}` | movement | Lifter |
| `uppity.lifter.seek.random` | `:PPR` | movement | Lifter |
| `uppity.lifter.home` | `:PH` | movement | Lifter |
| `uppity.lifter.home.speed` | `:PH{speed}` | movement | Lifter |
| `uppity.estop` | `:PX` | **power** | Power |
| `uppity.rotary.spin` | `:PR{speed}` (−100…100, 0 = home) | movement | Rotary |
| `uppity.rotary.spin.random` | `:PRR{max}` | movement | Rotary |
| `uppity.rotary.abs` | `:PA{degrees}` (0–359) | movement | Rotary |
| `uppity.rotary.abs.speed` | `:PA{degrees},{speed},{maxspeed}` | movement | Rotary |
| `uppity.rotary.abs.random` | `:PAR` | movement | Rotary |
| `uppity.rotary.rel` | `:PD{degrees}` (−180…180) | movement | Rotary |
| `uppity.rotary.rel.random` | `:PDR` | movement | Rotary |
| `uppity.movemode.auto` | `:PM` | movement | Sequences |
| `uppity.movemode.aggr` | `:PM{level}` (G/M/A) | movement | Sequences |
| `uppity.movemode.params` | `:PM,{liftSpeed},{rotSpeed},{minInterval},{maxInterval}` | movement | Sequences |
| `uppity.light` | `:PL{mode}` (0–7) | cosmetic | Lighting |
| `uppity.light.random` | `:PLR` | cosmetic | Lighting |
| `uppity.wait` | `:PW{seconds}` (1–60) | cosmetic | Sequences |
| `uppity.wait.random` | `:PWR{max}` (1–60) | cosmetic | Sequences |
| `uppity.cfg.calibrate` | `#PSC` | **movement** | Setup |
| `uppity.cfg.setId` | `#PID{id}` (0–99) | config | Setup |
| `uppity.cfg.zero` | `#PZERO` | config | Config |
| `uppity.cfg.factory` | `#PFACTORY` | config | Config |
| `uppity.cfg.restart` | `#PRESTART` | power | Power |
| `uppity.cfg.debug` | `#PDEBUG{state}` | config | Config |
| `uppity.cfg.wifiReset` | `#PWIFIRESET` | config | Setup |
| `uppity.cfg.wifi` | `#PWIFI{state}` | config | Setup |
| `uppity.cfg.aggression` | `#PAGGRESSION{level}` (0/1/2) | config | Setup |
| `uppity.cfg.motor` | `#PMOTOR{type}` (0 Greg 6.3:1 / 1 Pololu 19:1 / 2 IA-Parts) | config | Setup |
| `uppity.cfg.expert` | `#PEXPERT{state}` — *"19:1 motor only — frame damage risk"* | config | Setup |
| `uppity.cfg.status` | `#PSTATUS` | cosmetic | Config |
| `uppity.cfg.config` | `#PCONFIG` | cosmetic | Config |
| `uppity.cfg.listSeq` | `#PL` | cosmetic | Config |
| `uppity.cfg.deleteSeq` | `#PD{seq}` | config | Config |
| `uppity.cfg.rc` | `#PRC{state}` | config | Setup |
| `uppity.cfg.rotaryToggle` | `#PR` (reboots) | config | Setup |
| `uppity.cfg.baud` | `#PBAUD{rate}` (1200–921600) | config | Setup |
| `uppity.cfg.lifterLimit` | `#P{polarity}` (NCL/NOL) | config | Setup |
| `uppity.cfg.rotaryLimit` | `#P{polarity}` (NCR/NOR) | config | Setup |

Enums: `uppity.lightMode` (0 Full Cycle random, 1 Off, 2 Obi-Wan blue, 3 Yoda green fade, 4 Sith red flash, 5 Search Light white, 6 Dagobah, 7 Sparkle), `uppity.aggressiveness` (G/M/A), `uppity.aggressionLevel` (0/1/2), `uppity.motorType`, `uppity.onOff`, `uppity.limitLifter`, `uppity.limitRotary`.

Note on `uppity.rotary.rel`: `commentLabel` says *"chain multiple for multi-rev — single call clamps ≤180° in fw 3.5.3"*.

**⚠️ Stale file in the periscope repo.** `.../R2UppitySpinnerV3/r2uppityspinner-alt.json` (20,425 B) is an **older schema variant** than the upstream/vendored one (25,265 B): it uses per-command `"group"` (e.g. `"Playback"`, `"Lifter"`) where the current schema uses `"category"`, and it lacks the component-level `categories[]` array. Upstream and Sabé's vendored copy are equivalent. Any new-firmware work that regenerates this file must target the **current** schema.

---

# E. WCBClient

`github.com/greghulette/WCBClient`, **MIT**. Two local copies: **1.9.10** at `~/Documents/Arduino/libraries/WCB_Client` (what Sabé builds against) and **1.13.0** vendored at `.../Roam-A-Dome_ALT/RadFirmware/libs/WCB_Client` (with a 41 KB README and `docs/DELIVERY_STATS_SPEC.md`). Facts below are from 1.13.0 unless noted.

**Transport: ESP-NOW.** Not UDP, not serial-to-a-WCB. The client *is* a peer on the WCB mesh. **Zero external dependencies** — only `WiFi.h` and `esp_now.h` (PololuMaestro is needed only if you use `WCBStream`).

### WiFi initialisation — the answer to your key question

`WCB_Client.cpp` `begin()`, lines ~92–140:

```cpp
// forcing STA-only would tear the AP down. WiFi.disconnect() only drops …
wifi_mode_t targetMode = apActive ? WIFI_AP_STA : WIFI_STA;
if (!WiFi.mode(targetMode)) { /* warn */ }
WiFi.disconnect();
// "SoftAP detected — running WIFI_AP_STA, …"
if (!apActive) esp_wifi_set_channel(_meshChannel, WIFI_SECOND_CHAN_NONE);
esp_wifi_set_mac(WIFI_IF_STA, _wcbMACs[_deviceID - 1]);
```

So, precisely:
- **It does call `WiFi.mode()` itself**, and it **detects an existing SoftAP** and chooses `WIFI_AP_STA` instead of `WIFI_STA`.
- **It calls `WiFi.disconnect()`** — it will drop an existing STA association to a router.
- **It pins the channel with `esp_wifi_set_channel()` only when there is no SoftAP.** With a SoftAP up it *warns* about a channel mismatch but never force-moves the AP (`_lastWarnedChannel`, `_checkMeshChannel()`).
- **It overwrites the station MAC** to `02:oct2:oct3:00:00:<deviceID>` via `esp_wifi_set_mac()`. Anything depending on the factory MAC (MAC filtering, DHCP reservations, MAC-derived device names) will not see the real MAC. Two devices sharing deviceID + octets get an identical MAC — never reuse an ID.

### **Can it coexist with a SoftAP + HTTP web server on the same ESP32? YES — explicitly supported.**

From the README's own compatibility table:

> | SoftAP (hosted web UI) + WCB on the same board | ✅ **Supported** — `begin()` preserves an existing/later SoftAP as `WIFI_AP_STA`. The AP owns the single radio's channel, so bring it up on the mesh channel (`WiFi.softAP(ssid, pass, 1)`); the library **warns** (never force-moves it) if the radio ends up elsewhere. Match a non-default fleet channel with `setMeshChannel()`. |

The rules that make it work:
1. Call `WiFi.softAP(ssid, password, channel)` **before** `begin()` (calling it after also works — `WiFi.mode()` ORs in `WIFI_AP` without dropping STA).
2. **AP and STA share one radio channel on the ESP32.** ESP-NOW rides whatever channel the radio is on, so the AP's channel must equal the fleet's mesh channel. Mismatch = packets silently dropped, no error.
3. Disable modem sleep after `begin()`: `esp_wifi_set_ps(WIFI_PS_NONE)` or `WiFi.setSleep(false)`.

**Documented limitations:**

| Want | Result |
|---|---|
| WCB ESP-NOW only | ✅ intended use |
| **SoftAP + web UI + WCB** | ✅ **supported** (as above) |
| **STA joining an external router** + WCB | ⚠️ only if all peers share that router's fixed channel; `begin()` still disconnects STA |
| A second, separate ESP-NOW network | ❌ single global `esp_now_register_recv_cb` — last registrant wins, the other stack goes deaf |
| Multiple `WCB_Client` objects | ❌ singleton, declare once at global scope |
| Factory MAC preserved | ❌ overwritten by design |

**This is the single most important finding for the periscope**, because the current firmware runs a SoftAP + web server. Sabé chose a *second chip* for WiFi to keep the S3 radio-clean; RAD chose to drop WiFi entirely. WCBClient itself says a SoftAP + web UI + mesh on **one** ESP32 is supported — the constraints are (a) the AP must sit on the mesh channel, (b) no joining an external router, (c) modem sleep off, (d) the MAC is not yours any more.

### API surface (public methods, `WCB_Client.h`)

```cpp
WCB_Client(uint8_t mac_oct2, uint8_t mac_oct3, const char* password,
           uint8_t wcb_quantity, uint8_t deviceID,
           WCBCommandCallback cmdCb, WCBStatusCallback statusCb = nullptr);

bool begin();  void update();          // update() every loop — heartbeats, offline detect, WDP, retries

bool send(uint8_t target_wcb, const char* command, bool ensured = true);
bool broadcast(const char* command, bool ensured = true);
bool sendRaw(uint8_t target_wcb, uint8_t target_port, …);   // Maestro bytes
bool sendKyber(const uint8_t* data, size_t len);
bool sendRawPacket(uint8_t target_wcb, const uint8_t* data, size_t len);

bool isOnline(uint8_t wcb_number);
void enableSpecialPeer(uint8_t id = WCB_SPECIAL_ID);   // default 20
bool isSpecialPeerOnline();
bool sendToSpecialPeer(const char* command, bool ensured = true);

void onCommand(WCBCommandCallback);      // (uint8_t senderID, const char* command)
void onStatusChange(WCBStatusCallback);  // (uint8_t wcbID, bool online)
void onRawPacket(WCBRawPacketCallback);
void onNeighbor(WCBNeighborCallback);    // WDP adverts
void onBulkBegin/Chunk/Complete(...);

void setChecksum(bool);  void setMeshChannel(uint8_t 1..11);
void setIdentity(const char* type, const char* fw, const char* hw, const char* caps);
void setPortLabel(uint8_t port, const char* label);
void setMaestroIds(const uint8_t* ids, uint8_t count, const uint32_t* bauds = nullptr);
void setTemporary(bool);
void setAutoJoin(bool);  void forgetPeer(uint8_t);  void clearLearnedPeers();
uint8_t neighborCount();  bool getNeighbor(...);
WCBPeerStats getPeerStats(uint8_t) / getAggregateStats();  uint32_t getBroadcastSent();  void resetStats();
void monitorSerial(HardwareSerial&, uint8_t target_wcb, char terminator='\n');
void monitorRaw(HardwareSerial&, uint8_t target_wcb, uint8_t target_port = 0, …);
```

**Addressing** is by numeric **device ID**, not name: 1..`wcb_quantity` are standard WCB slots (pre-registered MACs); **20 is the out-of-band "special" slot**, needs `?SPECIAL,ON` on the WCBs, doesn't consume a WCB number. Names arrive later via **WDP** (`setIdentity` advertises `type`/`fw`/`hw`/`caps`; `onNeighbor`/`getNeighbor` consume adverts to learn alias, firmware, capabilities, controller link and Maestro IDs). Auto-join is **on by default** and learns any node heard **twice** above the `wcb_quantity` floor, persisting it to NVS key `wcb_peers`.

**Timing:** `_heartbeatIntervalSec = 10`, `_missedBeforeOffline = 5` → a 50 s offline window (widened from 3/30 s so a couple of lost broadcast heartbeats don't flap a live board out of the roster).

**Message length:** `structCommand[200]` = 199 chars + NUL; with checksum the `|CRC%08X` suffix (12 chars) shares that field → **187 usable on broadcast**. Unicasts auto-fragment to ~2.8 KB via MGMT chunks.

**Examples shipped:** `BasicUsage`, `AllFeatures`, `CombinedUsage`, `MaestroBroadcast`, `MaestroUnicast`, `MgmtRelay`, `NeighborDiscovery`, `SpecialPeer`.

### The WCB ecosystem wire format

Not `#WCB1:` — it is the `;`/`?` grammar from Greg's firmware Readme (`.../WCB/Sketches/Legacy/Upgrade - Nov 2025/Wireless_Communication_Board-WCB-main/Readme.md`, §2), quoted in A6 above. `?` = board-local config, `;` = execution/transfer, `^` = chain. A worked example straight from that Readme, with the WCB's own log:

```
;W3;S4:PP100^;M21^;T2500,;W3;S2#SD0^<SH1>^;T500,;W3;S1:PDA180
```
```
Processing input from Serial0: ;W3;S4:PP100^;M21^…
[TimerGroup 1] Executing command: ;W3;S4:PP100
[TimerGroup 1] Executing command: ;M21
[TimerGroup 2] Executing command: ;W3;S2#SD0
[TimerGroup 3] Executing command: ;W3;S1:PDA180
```

**Note that `;W3;S4:PP100` in Greg's own documentation is a periscope command** — the periscope is reached today as *"WCB3, Serial 4"*. A mesh-native periscope replaces that indirection: it would join as its own device ID and receive `:P…` directly, exactly as RAD did for `:DP…`.

**WCB firmware floor:** any new board joining the fleet must be on **WCB fw ≥ 6.2.0** or device 20 is invisible to it (`.../Sabe/docs/wcb-citizenship-audit-2026-08-10.md`). WCBClient requires WCB fw ≥ 6.0 generally.

**Sabé is pinned to WCBClient 1.9.10** and has an audited-but-not-installed bump to 1.15.0 (`.../Sabe/docs/wcbclient-bump-audit-2026-08-16.md`) — 1.14/1.15 add stored-sequence *names* (`seqHash`), *values* and *writes*, which is the prerequisite for a live sequence picker. Greg has tested only the NAMES path; Sabé would be the first consumer of value/write. RAD already vendors 1.13.0. **A new periscope firmware should vendor a pinned copy the way RAD does, rather than depend on whatever is in `~/Documents/Arduino/libraries/`.**

---

## Loose ends and flags

- **`https://github.com/highfalutintodd/sabe` is not publicly reachable** (404 on the API, absent from the public repo list). Everything in section A comes from the local clone; if the plan cites URLs, cite paths instead.
- **The Signal Booster README documents none of the board protocol** — I verified this directly. The integration contract lives in Travis's `droidnet_victron_battery_stats.txt` (local copy under Sabé's `docs/`) and in the 2.2.2 Python source, both cited above.
- The local Signal Booster payload is **2.2.2**; the newest public release is **2.1.3**. Facts drawn from 2.2.2 may be ahead of what other builders are running.
- **`libraryVersion` skew is real**: upstream 4.2.0 (21 boards) · Sabé's vendored 4.0.0 (19 boards incl. `sabe-extras`) · Signal Booster's bundled 2.1.0 (8 boards). Any UI that ships a vendored catalog needs the update path, not just the snapshot.
- The **stale `r2uppityspinner-alt.json` in the periscope repo root** uses the pre-`category` schema and should not be treated as current.
- RAD's `&RAD,HB` period is **hardcoded at 10 s** (`WcbLink.cpp`); Sabé's is settable 5–300 s. Mesh congestion has already caused real damage on this droid (garbled H-CR audio, `docs/hcr-s5-truncation-handover-2026-08-19.md`) — a periscope heartbeat should be settable from day one, and its telemetry should unicast to the special peer rather than broadcast.
- **Unverified:** I did not open Greg's OTA branch of the WCB firmware, so the "broadcast without a raw mapping" path is only as good as Travis's note (which itself says "implemented but untested").
