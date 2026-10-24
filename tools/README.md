# tools

- `release.sh` — builds the files a release carries, from a clean compile with the pinned
  `uppity` profile, into `~/Desktop/uppity-spinner-alt-<version>/` (or a folder you name):
  the full image (`0x0`, erases), the update for the page, the four files for an upgrade from v3
  that keeps v3's saved settings, `SHA256SUMS.txt` and `FLASHING.txt`. It checks the layout
  before it finishes.
- `droidnet/` — the DroidNet Command Library board file for 4.0 (`r2uppityspinner-alt.json`) and
  how it was validated.

v3's stored sequences are not carried over by a tool in the beta: note them on v3 with `#PL` and
type them back in on v4 (README, "Installing").
