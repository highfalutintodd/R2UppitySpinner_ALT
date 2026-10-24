#!/bin/bash
# Builds the files a release carries (docs/REWRITE-PLAN.md, Phase 6), from a clean compile of
# UppityFirmware with the pinned `uppity` profile. Nothing is written inside the repo.
#
#   tools/release.sh                      -> ~/Desktop/uppity-spinner-alt-<version>/
#   tools/release.sh /some/other/folder   -> that folder (created; must not already exist)
#
# What comes out, and what each is for (also written into FLASHING.txt next to them):
#   uppity-spinner-alt-<v>-full.bin       a fresh board, or starting over: flash at 0x0. ERASES all settings.
#   uppity-spinner-alt-<v>-update.bin     a board already on v4: Setup -> Firmware on the page. Keeps settings.
#   upgrade-from-v3/1..4-*.bin            a board on v3: four files at their addresses. Keeps v3's saved
#                                         settings, which v4 copies once on its first boot.
#   SHA256SUMS.txt                        checksums of everything above.
#
# Needs arduino-cli with the ESP32 core 3.3.4 (see UppityFirmware/sketch.yaml) and python3.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(sed -n 's/^#define UPPITY_FW_VERSION "\(.*\)"/\1/p' "$ROOT/UppityFirmware/Version.h")"
[ -n "$VERSION" ] || { echo "release: could not read UPPITY_FW_VERSION from UppityFirmware/Version.h" >&2; exit 1; }
NAME="uppity-spinner-alt-$VERSION"
OUT="${1:-$HOME/Desktop/$NAME}"
[ -e "$OUT" ] && { echo "release: $OUT already exists; move it away or name another folder" >&2; exit 1; }

DATA="$(arduino-cli config get directories.data 2>/dev/null || echo "$HOME/Library/Arduino15")"
BOOT_APP0="$DATA/packages/esp32/hardware/esp32/3.3.4/tools/partitions/boot_app0.bin"
[ -f "$BOOT_APP0" ] || { echo "release: boot_app0.bin not found at $BOOT_APP0 (is the ESP32 core 3.3.4 installed?)" >&2; exit 1; }

if [ -n "$(git -C "$ROOT" status --porcelain -- UppityFirmware webui 2>/dev/null)" ]; then
    echo "release: note: UppityFirmware/ or webui/ has uncommitted changes; this build includes them" >&2
fi
COMMIT="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"

WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
echo "Building $VERSION (commit $COMMIT)..."
arduino-cli compile --profile uppity --build-path "$WORK/build" --output-dir "$WORK/out" "$ROOT/UppityFirmware" | grep -E "Sketch uses|Global variables"

mkdir -p "$OUT/upgrade-from-v3"
cp "$WORK/out/UppityFirmware.ino.merged.bin"     "$OUT/$NAME-full.bin"
cp "$WORK/out/UppityFirmware.ino.bin"            "$OUT/$NAME-update.bin"
cp "$WORK/out/UppityFirmware.ino.bootloader.bin" "$OUT/upgrade-from-v3/1-bootloader-at-0x1000.bin"
cp "$WORK/out/UppityFirmware.ino.partitions.bin" "$OUT/upgrade-from-v3/2-partitions-at-0x8000.bin"
cp "$BOOT_APP0"                                  "$OUT/upgrade-from-v3/3-boot_app0-at-0xe000.bin"
cp "$WORK/out/UppityFirmware.ino.bin"            "$OUT/upgrade-from-v3/4-firmware-at-0x10000.bin"

# The layout the instructions promise: the pieces sit where the full image puts them, saved settings
# (nvs) stay at 0x9000 as on v3's default layout, and the firmware fits its 1.9 MB slot.
python3 - "$OUT" "$NAME" <<'EOF'
import struct, sys, os
out, name = sys.argv[1], sys.argv[2]
rd = lambda p: open(os.path.join(out, p), "rb").read()
full, app = rd(name + "-full.bin"), rd(name + "-update.bin")
boot, parts = rd("upgrade-from-v3/1-bootloader-at-0x1000.bin"), rd("upgrade-from-v3/2-partitions-at-0x8000.bin")
table = {}
for i in range(0, len(parts), 32):
    e = parts[i:i + 32]
    if e[:2] != b"\xaa\x50": break
    off, size = struct.unpack("<II", e[4:12])
    table[e[12:28].rstrip(b"\0").decode()] = (off, size)
problems = []
if len(full) != 4 * 1024 * 1024: problems.append("the full image is not 4 MB")
if full[0x1000:0x1000 + len(boot)] != boot: problems.append("bootloader not at 0x1000 in the full image")
if full[0x8000:0x8000 + len(parts)] != parts: problems.append("partition table not at 0x8000 in the full image")
if full[0x10000:0x10000 + len(app)] != app: problems.append("firmware not at 0x10000 in the full image")
if table.get("nvs", (0, 0))[0] != 0x9000: problems.append("settings (nvs) not at 0x9000: a v3 upgrade would lose them")
if table.get("app0", (0, 0)) != (0x10000, 0x1E0000): problems.append("app0 is not the min_spiffs 0x10000/0x1E0000 slot")
if len(app) > 0x1E0000: problems.append("the firmware is larger than its slot")
if problems:
    print("release: LAYOUT CHECK FAILED: " + "; ".join(problems), file=sys.stderr); sys.exit(1)
print("Layout checked: bootloader 0x1000, partitions 0x8000, settings 0x9000, firmware 0x10000 (%d of %d bytes)" % (len(app), 0x1E0000))
EOF

cat > "$OUT/FLASHING.txt" <<EOF
Uppity Spinner ALT $VERSION (commit $COMMIT)

Pick ONE of the three ways below. The README in the repository explains each step in more detail.

1. The board already runs v4  ->  uppity-spinner-alt-$VERSION-update.bin
   On the board's web page: Setup -> Firmware -> Choose File -> this file -> Upload.
   Keeps every setting. The page refuses while a motor is running.

2. A fresh board, or you want to start over  ->  uppity-spinner-alt-$VERSION-full.bin
   Flash it at address 0x0 over USB. This ERASES everything saved on the board; you then
   run commissioning (Setup -> Commissioning) before the motors will move.
     Browser (Chrome or Edge): https://espressif.github.io/esptool-js/  -> Connect -> pick the
       board's port -> Flash Address 0x0, choose the file -> Program.
     Command line: esptool.py --chip esp32 --port <your port> write_flash 0x0 uppity-spinner-alt-$VERSION-full.bin

3. The board runs v3 (3.x) and you want v4 to copy its saved settings  ->  upgrade-from-v3/
   Flash all four files in one go, each at the address in its name. v3's saved settings sit
   between them and are left alone; v4 copies the ones it trusts on its first boot and says so
   ("V3 import: …"). v3's stored sequences are NOT copied: write them down first (#PL on v3).
     Browser: the same page as above, with four rows: 0x1000, 0x8000, 0xe000, 0x10000.
     Command line:
       esptool.py --chip esp32 --port <your port> write_flash 0x1000 1-bootloader-at-0x1000.bin 0x8000 2-partitions-at-0x8000.bin 0xe000 3-boot_app0-at-0xe000.bin 0x10000 4-firmware-at-0x10000.bin
   Then run commissioning: v4 re-measures the travel, the rotary and the safe-spin height.

Going back to v3: flash v3.6.0 the way you always have. v4's settings are stored apart from
v3's and do not disturb it.

Check a download against SHA256SUMS.txt:  shasum -a 256 -c SHA256SUMS.txt
EOF

( cd "$OUT" && shasum -a 256 "$NAME-full.bin" "$NAME-update.bin" upgrade-from-v3/*.bin > SHA256SUMS.txt )
echo "Release files for $VERSION are in: $OUT"
ls -l "$OUT" "$OUT/upgrade-from-v3"
