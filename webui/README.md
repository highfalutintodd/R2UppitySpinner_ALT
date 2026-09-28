# webui

The v4 web interface. Plain HTML/CSS/JS in one file (`index.html`), no build framework.

- `python3 webui/build-assets.py` gzips it into `UppityFirmware/src/arduino/WebAssets.h` (committed;
  run it after any edit, then recompile). The build id it stamps becomes the page's ETag.
- `python3 webui/dev-server.py` serves the page at http://127.0.0.1:8765 against a simulated
  periscope (same `/api/*` contract as `Net.cpp`, and the same kind of log lines as the console,
  so the page's reply matching is exercised) so every page can be tried in a browser before any
  flash. It is a stand-in for layout and flow checks, not a model of the motion core; the host
  tests in `UppityFirmware/test/native` are that.

Design: docs/REWRITE-PLAN.md §4 (Sabé's tokens; paper theme by default, dark is a toggle).
Phase 4 pages: Operate, Setup (commissioning, motor & calibration, tuning, serial, WiFi, firmware,
theme), Diagnostics (live sensors, rotary test, log, configuration text), Rescue (override,
hold-to-jog, set home here, clear fault). Sequences are parked.

How the page talks to the board:
- `GET /api/state` every 600 ms (diffed; three misses = NO LINK) and `GET /api/config` when Setup
  or Rescue opens and after each save.
- One tail of `GET /api/log?since=` feeds the log view and every "send and wait for the reply"
  (a settings save waits for the board's own `#PKEY=… (saved)` line).
- Everything that changes something is a `#P`/`:P` line (`POST /api/cmd`, or `POST /api/config`
  for a key), so it lands in the log like a serial command. The exceptions are the E-STOP (its own
  four channels, unchanged since Phase 3), the soft stop, the update upload, and hold-to-jog
  (`POST /api/jog` renews a 2 s lease every 500 ms while the button is held; `/api/jog/stop` ends
  it on release).
