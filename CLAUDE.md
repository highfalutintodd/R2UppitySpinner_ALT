# R2 Uppity Spinner ALT — project guide for Claude Code

ESP32 firmware for the **Uppity Spinner** periscope lifter PCB in an R2-D2 astromech
dome. This repo (`highfalutintodd/R2UppitySpinner_ALT`) is a **community fork of
[reeltwo/R2UppitySpinnerV3](https://github.com/reeltwo/R2UppitySpinnerV3)** — a drop-in
replacement that preserves every original Marcduino serial command while adding a rebuilt
web UI, safer rotary homing, pulsed soft-stops, aggressiveness levels, and a serial ESTOP.

Maintainer: **Todd Word** (GitHub `highfalutintodd`, Discord "Highfalutintodd"). Current
firmware: **v3.5.3** (`FIRMWARE_VERSION` in `R2UppitySpinnerV3.ino:103`).

---

## ⚠️ Hard rules — read before touching anything

1. **This is running production firmware on real, hard-to-tune hardware.** It took a long
   time to get working and the owner does not want it broken. Do **not** make invasive
   changes to working motion/serial code without an explicit, stated regression-risk
   assessment and the owner's go-ahead. When in doubt, propose — don't edit.
2. **Never write to files outside this repo without saying so.** In particular the patched
   library at `~/Documents/Arduino/libraries/Reeltwo/` is load-bearing for the owner's
   builds. For upstreaming work, operate in a throwaway clone under the scratchpad/`/tmp`,
   never in the local library or this sketch's working tree.
3. **Public actions need explicit per-action approval.** Opening PRs, pushing branches,
   posting to Discord/GitHub — confirm first, every time. One approval does not carry to
   the next action.
4. **Motion-safety invariants are not cosmetic.** Every descent homes the rotary first
   (`:PH` ordering); pulsed drive exists for lead-screw soft-stops; ESTOP checks live
   inside movement loops. Don't "simplify" these away. See the pulsed-drive nuance in
   memory — it is required for soft-stop deceleration but *harmful* for slow creep on
   high-reduction motors.
5. **Tone:** answer neutrally, no condescending closers, no "did you not realize" framings.
6. Persistent working context (feedback, open bugs, backlog, this session's history) lives
   in the **memory system** — see the bottom of this file. Check it; it auto-loads an index.

---

## Repositories & where things live

| Thing | Location | Notes |
|---|---|---|
| **This sketch (ALT)** | `highfalutintodd/R2UppitySpinner_ALT` | **Not** a GitHub fork of reeltwo (standalone repo, no parent) — so no "Sync fork". Working dir is the local clone. |
| Upstream sketch | `reeltwo/R2UppitySpinnerV3` | The stock firmware this forked from. New maintainer: `thePunderWoman` (in the reeltwo org). |
| ReelTwo library (stock) | `reeltwo/Reeltwo` | Arduino library dependency. |
| ReelTwo library (local, **patched**) | `~/Documents/Arduino/libraries/Reeltwo/src/` | Patched for ESP32 core 3.x / ESP-IDF 5.5. **Do not overwrite.** Status of each patch (some now upstream) is tracked in memory `project_reeltwo_esp32_fixes`. |
| DroidNet command library | `travisccook/droidnet-command-library` | Shared board catalog; the ALT board file `r2uppityspinner-alt.json` was contributed via PR #1. |

Working directory note: the repo lives under an iCloud path with **spaces** in it —
always quote paths. `build/` and `.claude/` are gitignored.

---

## Build & flash

Two build systems are configured (use whichever the task calls for — neither has been
run by Claude in-session, so treat first invocation as unverified):

- **Makefile** (reeltwo Arduino.mk system): `make` / `make flash`. Targets ESP32,
  `PORT=/dev/ttyUSB0`, deps `reeltwo/Reeltwo`, `adafruit/Adafruit_NeoPixel`,
  `reeltwo/PCF8574`.
- **PlatformIO** (`platformio.ini`): env `uppity-spinner`, board `esp32dev`,
  `pio run` / `pio run -t upload`, monitor at 115200.

**`arduino-cli` IS installed** (1.5.1 via Homebrew) with the `esp32:esp32` core and all
three required libraries, so Claude *can and should* verify a build before handing code
over:

    arduino-cli compile --fqbn esp32:esp32:esp32 .

Pass `--build-path` somewhere under the scratchpad to keep artifacts out of the repo, and
re-use that path for fast incremental rebuilds. To confirm a change's size impact, compile
the pre-change file the same way and diff the "Sketch uses N bytes" line.

**Headroom is tight: a clean build sits at ~94% of program storage** (~1.24 MB of
1.31 MB). Always check the size line on any change that adds code.

Claude still cannot flash or test on hardware — the owner does that. Write test plans as
"compiles clean; behaviour verified on the owner's hardware," never "CI-verified."

---

## Firmware orientation

Single large sketch: `R2UppitySpinnerV3.ino` (~6.4k lines). Web assets in `WebPages.h`
(~2.7k lines) + `web-images.h`; screen/menu bits in `Screens.h` / `menus/`; pin
assignments in `pin-map.h`. Two TB9051FTG motor drivers (lifter + rotary), serial
(Marcduino `:P*` / `#P*` grammar) and a WiFi web UI.

**Serial command pipeline (this fork has diverged hard from upstream here):**
- RX callbacks `onUsbSerialReceive` / `onCmdSerialReceive` run an **ESTOP FSM
  (`advanceEstopFsm`) ahead of the buffer**, then push bytes into software FIFOs
  (`sUsbFifo` / `sCmdFifo`). This is why a mid-chain `:PX` is caught even if the command
  buffer is busy — a property upstream lacks.
- `loop()` drains the FIFOs into `sBuffer`; `runSerialCommand()` dispatches.
- Chained `:P*` commands: the programmatic chain path uses a **separate `sCmdBuffer`**
  (added in v3.5.2/3.5.3 to fix chains silently no-op'ing after the first command); the
  serial-triggered path still dispatches from `sBuffer`.
- `sProcessing` is true for the whole life of a chain (including `:PW` waits).

Full command reference and chain-ordering rules are in `README.md` ("Serial Command
Reference"). Key safety commands: `:PX` (serial ESTOP), `:PH` (raise→home rotary→lower),
`:PM[G/M/A]` (random mode + aggressiveness).

---

## Conventions

- **Versioning:** `FIRMWARE_VERSION` semver in the `.ino`; bump with the change, note it
  in README/commit. Recent tags follow `vX.Y.Z — one-line summary`.
- **Commits:** end firmware/library commits with the Co-Authored-By trailer for Claude
  (see git log). Keep the README's plain, builder-facing voice — explain the *why*
  (hardware reality), not just the *what*.
- **Upstream contributions** (reeltwo/*): match the maintainer's house style — she keeps
  **both** code paths behind `#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5,0,0)` /
  `ESP_ARDUINO_VERSION` guards and never drops the legacy branch. One file per PR,
  independent branches off `master`, honest test plans.
- **DroidNet board catalog:** new/unverified boards use `confidence: "community"`; bump
  `libraryVersion` (minor for a new board) and keep `releases.json` in lockstep;
  `npm run validate` + `npm test` before submitting.

---

## Active cross-session work

The current thread of work is **upstreaming ALT improvements** back to reeltwo and the
DroidNet catalog. Live status board: **[docs/UPSTREAMING.md](docs/UPSTREAMING.md)** —
check it at the start of any session that continues this effort.

---

## Memory

Persistent context lives in the memory system at
`~/.claude/projects/-Users-toddword-...-R2UppitySpinnerV3/memory/` (auto-loaded index in
`MEMORY.md`). It holds: the ReelTwo library patch/upstream status, open periscope bugs,
backlog items (seek-mode selector, rotary angle drift, `:PD` multi-rev clamp, dark mode),
and working-style feedback. Prefer updating memory over duplicating that state here.
