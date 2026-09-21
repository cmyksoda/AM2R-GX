#!/usr/bin/env bash
# Assemble a ready-to-copy SD tree for AM2R GX (bring-your-own data.win).
# Output is a self-contained apps/ folder you drop onto the SD card root, so
# it becomes sd:/apps/am2r/... — no mount path needed here.
#
# Usage: tools/sd-pack.sh [SRC_EXTRACT_DIR] [OUT_DIR]
#   SRC_EXTRACT_DIR  your AM2R extract (has data.win + lang/)      [working/extracted/win11]
#   OUT_DIR          where the SD tree is built                    [dist/sd]
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${1:-$REPO/working/extracted/win11}"
OUT="${2:-$REPO/dist/sd}"
DOL="$REPO/build/wii/boot.dol"
HBC="$REPO/wii/hbc"
APP="$OUT/apps/am2r"

die() { echo "sd-pack: $*" >&2; exit 1; }

[ -f "$SRC/data.win" ]           || die "no data.win in $SRC (point arg 1 at your AM2R 1.1 extract)"
[ -f "$SRC/lang/english.ini" ]   || die "no lang/english.ini in $SRC"
[ -f "$DOL" ]                    || die "no $DOL — run 'make wii' first"

mkdir -p "$APP/game/lang" "$APP/save"
cp -f "$DOL" "$APP/boot.dol"
cp -f "$HBC/meta.xml" "$APP/meta.xml"
cp -f "$HBC/icon.png" "$APP/icon.png"
cp -f "$SRC/lang/languages.txt" "$SRC/lang/english.ini" "$APP/game/lang/"
# The Wii's GPU binds nothing over 1024x1024: a data.win with bigger texture pages (Community
# Update 1.5.x) is repacked losslessly and proven so; 1.1 already fits and is copied untouched.
if python3 "$REPO/tools/repack_textures.py" "$SRC/data.win" --check; then
	python3 "$REPO/tools/repack_textures.py" "$SRC/data.win" "$APP/game/data.win"
	python3 "$REPO/tools/verify_repack.py" "$SRC/data.win" "$APP/game/data.win" | tail -n 1
else
	rsync -a "$SRC/data.win" "$APP/game/data.win"
fi

echo "SD tree ready: $OUT"
echo "  copy its apps/ folder onto the SD card root, then launch AM2R GX from HBC."
echo "  (music .ogg not packed — audio backend is 'none' until Phase F.)"
( cd "$OUT" && find apps/am2r -maxdepth 3 -type f | sort | sed 's/^/    /' )
