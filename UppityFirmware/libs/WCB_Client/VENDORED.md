# WCB_Client, vendored

Greg Hulette's WCB_Client (MIT, https://github.com/greghulette/WCBClient), copied unchanged
from commit `534a6c9` (2026-10-04, "1.17.1: boot announces both ways, best-effort JSON
broadcasts, config parts through WcbMgmt and MgmtRelay"). Todd asked for the newest; 1.17.1 is
not tagged yet (the last tag is 1.13.2, which is also the newest in the Arduino library index).

Only `src/`, `library.properties`, `LICENSE`, `README.md` and `keywords.txt` are copied; the
examples and the upstream design notes are left out. The `uppity` profile in `../../sketch.yaml`
builds against this folder (`- dir: libs/WCB_Client`), so every build uses exactly this code.

To update: copy the same files from a newer commit, note it here, rebuild, and re-run the mesh
part of the bench procedure (docs/BENCH-PHASE5.md).
