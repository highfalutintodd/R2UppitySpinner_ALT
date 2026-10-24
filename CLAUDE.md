# R2 Uppity Spinner ALT — project guide for Claude Code

ESP32 firmware for the **Uppity Spinner** periscope lifter PCB in an R2-D2 astromech dome.
Maintainer: **Todd Word** (GitHub `highfalutintodd`, Discord "Highfalutintodd").

**This branch is the v4 clean-sheet rewrite.** Two trees coexist here:

| Tree | What | Rule |
|---|---|---|
| `UppityFirmware/` | **v4** — the rewrite. Arduino sketch + `src/core` (pure C++), `src/sim` (mechanism simulator), `src/arduino` (glue), `test/native` (host tests). | All new work goes here. |
| `R2UppitySpinnerV3.ino`, `WebPages.h`, `menus/`, `pin-map.h`, `libraries/` | **v3.6.0** — the patched-upstream firmware, frozen. Kept in the tree during the rewrite as the behavioural reference; removed at release (Phase 6). Tag `v3.6.0` and `main` are the shipping v3 line. | **Do not edit.** Read it, never patch it. |

Approved plan and the only design authority: **[docs/REWRITE-PLAN.md](docs/REWRITE-PLAN.md)**
(Part 1 plain English, Part 2 technical, Appendix F motion core). Supporting references:
[docs/MOTION-CORE-DESIGN.md](docs/MOTION-CORE-DESIGN.md), [docs/v3-inventory/](docs/v3-inventory/)
(firmware inventory, web UI inventory and critique, ecosystem facts), and the older
[docs/REWRITE-HANDOFF.md](docs/REWRITE-HANDOFF.md) (hardware reality and the September 2026
debugging record). Read the plan's §1.1 decisions table before proposing anything.

---

## Hard rules

1. **No runaway periscope.** The head never descends into the dome unless the rotary is home; the
   rotary never turns unless the head is above the safe-spin height; with unknown lifter position
   only upward motion is allowed; any context may stop the motors, only the supervisor task may
   drive them. These are implemented in one place (`SafetyPolicy`) and never bypassed by a script.
2. **Pure core vs Arduino glue.** `src/core` and `src/sim` include no Arduino headers and compile on
   the Mac. Hardware, FreeRTOS, `Wire`, NVS and `Serial` live only in `src/arduino` and the `.ino`.
3. **Definition of done, every change:**
   `make -C UppityFirmware/test/native test` passes and
   `arduino-cli compile --profile uppity UppityFirmware` is clean, with the size line recorded.
   Use `--build-path` under the session scratchpad; never leave build output in the repo.
4. **Say what you actually tested.** "Host tests pass, compiled, not on hardware" is a complete
   and welcome report. Claude cannot flash or run hardware; Todd does, from a written procedure.
   Never write "verified" for something only the simulator ran.
5. **Only the supervisor task writes a non-zero PWM. Only `I2cService` touches `Wire`. Only
   `Persist` touches NVS, and never while motors are enabled.** RX callbacks flag or copy, nothing
   else.
6. **Settings are append-only** with a schema version and migrations; tuning defaults have one
   source (Settings → sanitize → parser → `#PCONFIG` → COMMANDS.md, all or none).
7. **Everything the board emits must parse as a silent no-op on every other board** (`KEY=VALUE`
   lines, `&PERI,…`), never as a command or as "Invalid" spam.
8. **Serial grammar is a contract.** Every v3 `:P*`/`#P*` command keeps its syntax; deltas are
   additive and listed in the plan's Appendix A.
9. **Public actions need explicit per-action approval:** pushes, PRs (reeltwo, DroidNet), Discord
   or GitHub posts. One approval does not carry to the next action. Commit only when asked.
10. **Tone:** neutral, no condescending closers, no "did you not realize" framings. Explain the
    reasoning, not just the result; Todd is a capable builder, not a professional developer.
11. **Never write outside this repo without saying so** (the patched ReelTwo library under
    `~/Documents/Arduino/libraries/` is load-bearing for v3 builds).
12. **No bench step may depend on a clearance that was not measured on Todd's hardware.** On
    2026-09-19 a step written against the firmware's default safe-spin height (half travel)
    turned the head at 54 % and hit the lifter frame; the periscope head came loose and the
    rotary drive has had play ever since. Heights, travel and switch positions come from the
    builder's own numbers (`#PV3` reads v3's NVS) or from a slow, watched measurement, never
    from a profile default. Until a clearance is set, only the top counts as clear.
    **Nor may a bench step rely on the firmware refusing a move next to an unverified clearance.**
    On 2026-10-03 a step said the board would hold a turned head at the safe-spin height; the next
    jog press was let through by a policy hole and the head hit the frame again. Stage every
    refusal test where the move would be harmless if the refusal failed (a bounded `#PNUDGE` or a
    short `:PP`, above a height the head is known to clear), never with a held jog.
13. **Todd is not a coder and not comfortable in the terminal.** Every instruction is a complete
    copy-paste command with the real path and the real port (`/dev/cu.wchusbserial110`), one per
    fenced block, with what success looks like. No placeholders.

---

## Build and test

```
make -C UppityFirmware/test/native test            # host tests (UBSan on; `make asan` optional)
arduino-cli compile --profile uppity UppityFirmware # ESP32, core 3.3.4 pinned in sketch.yaml
```
Profile `uppity` = `esp32:esp32:esp32:PartitionScheme=min_spiffs,FlashSize=4M`. Baseline at
Phase 0: 314 KB of 1,966,080 (15 %); end of Phase 1 bench: 381 KB (19 %); Phase 2: 408 KB; Phase 3 (WiFi): 1,139 KB (57 %);
Phase 4 (full page, 24 KB gzipped): 1,176 KB (59 %); code review 2026-10: 1,181,615 B (60 %); Phase 5 (mesh + WCB_Client 1.17.1): 1,225,487 B (62 %); AP-name fix 2026-10-24: 1,225,495 B; mesh pacing + hints: 1,227,167 B. Paths under this repo
contain spaces: always quote them. `arduino-cli` 1.5.1 is installed via Homebrew; the ESP32
core 3.3.4 is installed; Apple Silicon needs Rosetta 2 for arduino-cli's `ctags` (installed
2026-09-19). Todd flashes from the Terminal app with the three commands at the top of
the current bench doc (`docs/BENCH-BETA.md`; installs through the page's Setup → Firmware upload, from the files `tools/release.sh` makes). Work lands on a session worktree branch;
Todd fast-forwards `v4-rewrite` in the main checkout himself (Claude's own merge attempts are
blocked by the tool policy): `git -C "<repo>" merge --ff-only <session branch>`.

Known local quirk: AddressSanitizer deadlocks in its own initialiser on this macOS build
(Darwin 25, Apple clang 17), so the default test build uses UBSan only.

Web page: `python3 webui/dev-server.py` (or the `uppity-dev` entry in `.claude/launch.json`,
which is gitignored and may need re-creating in a new worktree) serves `webui/index.html` against
a simulated board at http://127.0.0.1:8765; the in-app browser can open it. After any page edit,
`python3 webui/build-assets.py` regenerates `WebAssets.h`, and `node --check` on the extracted
script catches syntax errors. The dev server is a stand-in for the API shape, not the motion core:
check new state fields against what `Net.cpp`/`Console.cpp` actually write.

---

## Phase status (update as phases land)

| Phase | Status |
|---|---|
| 0 Skeleton, simulator, tests, docs | **Done** 2026-09-07: tree, profile, core foundation, simulator, ESP32 HAL (PCNT + 10 kHz LEDC), TB9051FTG facts in docs/TB9051FTG-NOTES.md. |
| 1 Motion core + USB console | **Bench-verified on Todd's 19:1 periscope 2026-09-19/20** (branch `v4-rewrite`): every step of docs/BENCH-PHASE1.md incl. calibration and the extended rotary set; 82 host tests; ESP32 build 381 KB. Fixes from the bench are listed in that doc's results section (limit-edge contradiction guard, `#PINVERT`/`#PTRAVEL`/`#PSAFESPIN`/`#PROTARYTPR`/`#PHOMEPARK`/`#PWIGGLE`/`#PV3` bench commands, `:PH` ends on the switch and homes after characterising, top-only safe-spin default, switch-only two-move park that reproduces v3's home spot, rest-switch hold filter, tick-walking short rotary moves). |
| 2 Command layer + settings | **Bench-passed 2026-09-20 evening** (commit `c0fc379`, fixes `e6e0602`): settings persist across a power cycle with a one-shot conservative v3 import, `#P<KEY>[=]<value>` + replayable `#PCONFIG`, command UART with its own assembler, non-blocking sequencer, stored sequences (Todd does not use them), random director, `#PRANDOMLIGHT`, trigger inputs, status LED. Todd's one-time lines after a fresh flash: `#PINVERT1 #PTRAVEL1150 #PROTARYTPR11855 #PUNLOCK` (safe-spin 904 comes from the v3 import). Four log findings fixed in `e6e0602` (nearest-zero home after a spin, TICK spam, home-if-needed for random, `:PP0` report); that fix build's re-test passed on the droid 2026-09-27 (all four fixes and `#PRANDOMLIGHT` confirmed; log in docs/BENCH-PHASE2.md results). Not yet tested: command UART, trigger inputs. 110 host tests; 408 KB. |
| 3 Network + minimal UI | **Bench-verified 2026-09-27** (commits `67e6587`…`7b35682`, build `0551078b`): AP always up, optional home network with backoff, captive DNS, mDNS `uppity.local`, web task on core 0 with a 400 ms stall guard, JSON API, one gzipped Operate+console page, 16 KB log ring, OTA (`espota.py`; refused while armed), settings schema 3. E-STOP from the page lands mid-move via a dedicated port-81 listener plus four page channels with retries; a new command interrupts a running seek or rotation, and the supervisor waits for rest after any interrupted job. Four bench runs and their fixes: docs/BENCH-PHASE3.md. Not yet tested: two phones, command UART, trigger inputs. |
| 4 Full web UI | **Complete 2026-10-04**, bench-passed on Todd's droid (docs/BENCH-PHASE4.md, results sections): four pages (Operate, Setup, Diagnostics, Rescue), Setup forms over every key, firmware upload from the page, Rescue (`#POVERRIDE` 60 s, hold-to-jog lease, `#PNUDGE`, `#PSETHOME`), commissioning (`CommissionFlow`, settings schema 4) run end to end (dome off). Bench fixes, each with a host test that fails on the old code: the policy hole that let a clamped turned head down (second frame strike, `turnedSinceHome_`); E-STOP brakes with the drivers on until still (EN low = coast; the mast free-wheeled down); creeps brake early by speed × the calibration's braking time (`brakeLeadMs`, `CALBRAKE`) and a creep-clamped turned head stays put (`clampHeld`); the rotary test ignores the switch it starts on and `#PROTARYTEST` homes after; calibration travel is measured between the switch closings (it was rest to rest, 1192 vs ~1138). **Dome-on commissioning 2026-10-04** (build `77cbfb02`): rotary 1185.0, travel 1137 between the switch closings (fix confirmed on the droid), safe-spin **878** measured with the dome on. Last fix from it: power-up on the bottom switch counted from the resting spot (~33 below the closing point, so the height read ~33 high until a switch); the calibration now records the deepest rest (`bottomRest`, calibration schema 2) and power-up starts the count at minus that. Re-test 4 passed (build `9743ad66`: `rest=37`, power-up at −37, top closed at 1131 for 1137; `#PTRAVEL0` in use). Free testing incl. Comlink that day found one more window: SwitchLive lags the switch 400 ms at rest (and while busy), so a head that coasted off the switch after a spin still counted as home; `rotaryIsHome` now needs the switch closed now for SwitchLive, else the count decides. 163 host tests; 1,176,727 B. Part E step 32 and Part F optional. Sequences page parked. |
| Code review 2026-10 | **Bench-passed 2026-10-04** (build 7d93e1d8; the restart wait and D2 not exercised by the timing, optional D2 re-test in the doc) (branch `claude/code-review-2026-10`): `docs/CODE-REVIEW-2026-10.md`. 27 defects fixed (a coasting head re-zeroed as home, the command UART deaf after `#PBAUD`, safe-spin against a guessed travel, the override for every source, rotary-off with a turned head, a stall from the switch, a stuck-closed switch, and smaller ones), each core fix with a test in `test_review.cpp`; rig ground-truth watch (`watchSafe`); UBSan now fails `make test`. Todd's D1-D4 done in a second commit (stale lifter count not trusted, Stop cancels the queue, random off after a stall, a jog the override allowed ends with it). 202 host tests; 1,181,615 B. Results at the end of the doc; D5 (short-seek braking) needs its own bench. |
| 5 Mesh + DroidNet | **Part A bench-passed 2026-10-24** (build `055c60e5`, installed from the page; results in `docs/BENCH-PHASE5.md`): presets within 4 ticks, every short step moves (34/23/11 ticks, within 8), 31 random seeks −7…+5, E-STOP −59/−62, the clamp at a borrowed 1100 rested at 1105 by the creep path (the seek path against the floor not exercised: optional 1050 re-run in the doc). Found: the AP name was built from an uninitialised buffer since Phase 3 (`WiFi.macAddress()` before `WiFi.mode()` writes nothing on core 3.3.4; `FB3F` through 2026-10-04, `FC3F` on 2026-10-24); fixed the same day with `esp_read_mac(…, ESP_MAC_WIFI_STA)`, so Todd's AP is `R2Uppity-1390` (on the droid, build `285d30a7`). **Part B bench-passed 2026-10-24** (build `285d30a7`, Todd's WCBs now on **7.0.1**, dome off so WCB3 unpowered): steps 8-18 (join, heartbeats on WCB1, WCBs learn board 5 above the quantity, `;W5#PSTATE`, settings refused, `;W5:PP100`, `;W5&ESTOP` latch + `&ESTOP,RESET`, `&DISABLE`/`&STATUS`/`&HELLO`, `;W5:PH`); step 19 (wire + mesh dedup) not run. Fixes after it (not yet on the droid): the board's own broadcasts paced 40 ms apart (`MeshTxPacer`; a `&STATUS` burst lost its middle line), and every Mesh hint now says `?BACKUP` (`?EPASS`/`?MAC`/`?WCBQ`/`?WCBCH` SET values on a WCB). 235 host tests; 1,227,167 B. Implemented 2026-10-04 (branch `claude/phase5-mesh`). **Part A (D5):** seeks brake by the speed reached (16 ms speed, carried forward by a quarter of the change while accelerating, times the calibration table's braking time at that speed); short steps always run the breakaway burst; every descent the policy gives a floor (head maybe turned) brakes against it with the creeps' margin (60 ms without a table) and, near it, is held (≤ 20 ticks) or creeps (≤ 60); a seek stopped at the floor holds the clamp; `SEEK came to rest at …` log line. An independent review found a target just *above* the floor could overshoot below it (fixed, tested). **Mesh:** off by default; WCB_Client 1.17.1 vendored (`libs/WCB_Client`, commit 534a6c9); `MeshProtocol.h` (pure: classify, E-STOP patterns, `#P` gate = `#PSTATE`/`#PSTATUS`/`#PCAL`/`#PHELP` + `#PRESUME` only, nothing that reads flash, `&PERI` HB/POS/STATE/FAULT/ACK/HELLO and timing); `Mesh.cpp` (radio task trips the latch and copies; loop() dispatches as unattended `ReqSource::Mesh`); mesh on = AP on the mesh channel, no home network; settings schema 5 (`MESH`, `MESHMAC`, `MESHPASS` secret, `MESHQTY`, `MESHID`, `MESHCHAN`, `MESHCRC`, `MESHHB`); dedup ring of 4; Setup → Mesh page; D8 (`#PROTARYTPR=0` when unmeasured). Todd's mesh: 3 WCBs fw 7.0.1 since 2026-10-24 (6.2.0 when Phase 5 was written), octets 3C:4E, ch 1, checksum on, Sabé = 20, **periscope = 5**; the periscope's command wire is **WCB2 serial port 2** (`;W2;S2`, found 2026-10-24; earlier notes said `;W3;S4`, which was wrong), and it stays. DroidNet Command Library 4.0 entry in `tools/droidnet/` (validated; the PR to Travis needs Todd's approval). A second review of the mesh found six defects (long or multi-line messages, the fault fallback, stale page status, channels 12-13, a leading space), all fixed with tests. 234 host tests; 1,225,487 B. Open: whether WCB 6.2.0 learns a board above its quantity (bench step 13); Sabé-side changes (separate repo, separate approval). |
| 6 Release | **4.0.0-beta.1 prepared 2026-10-24, awaiting the pre-beta bench** (`docs/BENCH-BETA.md`). Todd's decisions that day: name **4.0.0-beta.1**; **keep the v3 files in the tree for now**; no v3 sequence carry-over tool (README says note them with `#PL`); **warn while the AP password is the default** (boot `WIFI WARNING` line + page banner via `wifi.apDefault`). Done: `Version.h`, boot line without "phase", `tools/release.sh` (full image at 0x0 = erases; `-update.bin` for the page; `upgrade-from-v3/` four files keeping NVS at 0x9000, layout self-checked; SHA256SUMS, FLASHING.txt), builder README for v4, `CHANGELOG.md` (v3 history moved there), `docs/COMMANDS.md`. Open: the pre-beta bench (command wire, wire+mesh dedup, mesh E-STOP during a descent, Sabé's E-STOP, the &STATUS fix, AP password, 30-min soak, a fresh install from the full image); then, each with Todd's approval at the time: push, make v4 the default branch (v3-maintenance from `v3.6.0`), GitHub pre-release with the release files, the DroidNet PR. After the beta: Sabé side, WCB_Client 1.18.1 + its TX watchdog (WCB 7.0 notes pair it with 1.18.1), D6/D7, a web PIN, the sequence editor; and small items found writing COMMANDS.md: a serial `#PS<n>:…:PX` line latches the E-STOP as it arrives (so would a `#PCONFIG` replay holding one), `&ESTOP,<reason>` latches only on the mesh, SSID/PASS/APPASS longer than their fields are cut without a word (the page limits them), `#PHELP` omits `#PJOG0` and the lifter jog percent, `:PM0` starts random mode, verbs match by prefix (`#PSTATEX`), the mesh refusal says "USB console or the page" though the command port accepts settings. |

Heartbeat tag for Phase 5: **`&PERI`** (Todd, 2026-10-04). Open items Todd owns: keep an eye on the
rotary drive play (coupler set screw / encoder disc) — homing is robust to it but it will not
improve by itself. Todd's hardware numbers live in memory `project_todd_periscope_hardware` and
in the "Todd's hardware" section of docs/BENCH-PHASE1.md.

---

## Ecosystem (see docs/v3-inventory/ECOSYSTEM.md for the facts)

Sabé (private, local at `…/Control/Amidala Claude Project/Sabe`) and Roam-A-Dome ALT
(`…/Roam-A-Dome/Roam-A-Dome_ALT`) are the architectural templates. WCB mesh via WCBClient
(library index has 1.13.2; pin in `sketch.yaml` at Phase 5). DroidNet Signal Booster taps the WCB
bus passively; the Command Library board file `r2uppityspinner-alt` is regenerated for 4.0 at
Phase 5 against the current schema (the copy in this repo's root is stale and uses `group`).

---

## Memory

Persistent context lives in the memory system (auto-loaded index `MEMORY.md`). The entry
`project_v4_rewrite` holds the approved decisions and phase status; update it when a phase lands.
