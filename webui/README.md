# webui

The v4 web interface. Plain HTML/CSS/JS in one file (`index.html`), no build framework.

- `python3 webui/build-assets.py` gzips it into `UppityFirmware/src/arduino/WebAssets.h` (committed;
  run it after any edit, then recompile). The build id it stamps becomes the page's ETag.
- `python3 webui/dev-server.py` serves the page at http://127.0.0.1:8765 against a simulated
  periscope (same `/api/*` contract as `Net.cpp`) so the page can be tried in a browser before any flash.

Design: docs/REWRITE-PLAN.md §4 (Sabé's tokens; paper theme by default, dark is a toggle).
Phase 3 ships the Operate page and a console; Phase 4 adds Sequences, Setup, Diagnostics and Rescue.
