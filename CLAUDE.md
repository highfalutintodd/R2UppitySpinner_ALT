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

---

## Build and test

```
make -C UppityFirmware/test/native test            # host tests (UBSan on; `make asan` optional)
arduino-cli compile --profile uppity UppityFirmware # ESP32, core 3.3.4 pinned in sketch.yaml
```
Profile `uppity` = `esp32:esp32:esp32:PartitionScheme=min_spiffs,FlashSize=4M`. Baseline at
Phase 0: 314 KB of 1,966,080 (15 %). Paths under this repo contain spaces: always quote them.
`arduino-cli` 1.5.1 is installed via Homebrew; the ESP32 core 3.3.4 is installed.

Known local quirk: AddressSanitizer deadlocks in its own initialiser on this macOS build
(Darwin 25, Apple clang 17), so the default test build uses UBSan only.

---

## Phase status (update as phases land)

| Phase | Status |
|---|---|
| 0 Skeleton, simulator, tests, docs | **Done** 2026-09-07: tree, profile, core foundation, simulator, ESP32 HAL (PCNT + 10 kHz LEDC), TB9051FTG facts in docs/TB9051FTG-NOTES.md. |
| 1 Motion core + USB console | **Code complete on host** 2026-09-07: policy, faults, lifter seek/creep/calibrate/hold, rotary move/spin/jog/home/characterise, composites (`:PH`, auto-raise), supervisor with programs, control task, I2C service, serial front with E-STOP scanner, console. 71 host tests pass; ESP32 build 367 KB. **Bench pending**: docs/BENCH-PHASE1.md. |
| 2 Command layer + settings | not started |
| 3 Network + minimal UI | not started |
| 4 Full web UI | not started |
| 5 Mesh + DroidNet | not started |
| 6 Release | not started |

Open items Todd owns: TB9051FTG datasheet check (PWM1=PWM2=low: coast or brake?) before
Phase 1 motion tests; heartbeat tag `&PERI` vs `&UPPITY` before Phase 5.

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
