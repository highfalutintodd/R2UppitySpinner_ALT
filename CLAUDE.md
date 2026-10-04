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
Phase 4 (full page, 24 KB gzipped): 1,176 KB (59 %). Paths under this repo
contain spaces: always quote them. `arduino-cli` 1.5.1 is installed via Homebrew; the ESP32
core 3.3.4 is installed; Apple Silicon needs Rosetta 2 for arduino-cli's `ctags` (installed
2026-09-19). Todd flashes from the Terminal app with the three commands at the top of
the current phase's bench doc (`docs/BENCH-PHASE4.md`; OTA with `espota.py`). Work lands on a session worktree branch;
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
| 4 Full web UI | **Complete 2026-10-04**, bench-passed on Todd's droid (docs/BENCH-PHASE4.md, results sections): four pages (Operate, Setup, Diagnostics, Rescue), Setup forms over every key, firmware upload from the page, Rescue (`#POVERRIDE` 60 s, hold-to-jog lease, `#PNUDGE`, `#PSETHOME`), commissioning (`CommissionFlow`, settings schema 4) run end to end (dome off). Bench fixes, each with a host test that fails on the old code: the policy hole that let a clamped turned head down (second frame strike, `turnedSinceHome_`); E-STOP brakes with the drivers on until still (EN low = coast; the mast free-wheeled down); creeps brake early by speed × the calibration's braking time (`brakeLeadMs`, `CALBRAKE`) and a creep-clamped turned head stays put (`clampHeld`); the rotary test ignores the switch it starts on and `#PROTARYTEST` homes after; calibration travel is measured between the switch closings (it was rest to rest, 1192 vs ~1138). **Dome-on commissioning 2026-10-04** (build `77cbfb02`): rotary 1185.0, travel 1137 between the switch closings (fix confirmed on the droid), safe-spin **878** measured with the dome on. Last fix from it: power-up on the bottom switch counted from the resting spot (~33 below the closing point, so the height read ~33 high until a switch); the calibration now records the deepest rest (`bottomRest`, calibration schema 2) and power-up starts the count at minus that. Re-test 4 passed (build `9743ad66`: `rest=37`, power-up at −37, top closed at 1131 for 1137; `#PTRAVEL0` in use). 161 host tests; 1,176,647 B. Part E step 32 and Part F optional. Sequences page parked. |
| 5 Mesh + DroidNet | **Next.** Start from `docs/PHASE5-BRIEF.md` (scope, the WCB fleet questions for Todd, heartbeat tag `&PERI`). |
| 6 Release | not started |

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
