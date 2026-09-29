#!/usr/bin/env bash
# Assemble a ready-to-copy SD tree for AM2R GX from an already-patched AM2R 1.5.5 folder, for development:
# players get the first-boot patcher instead. Output is a self-contained apps/ folder for the SD card root.
#
# Usage: tools/sd-pack.sh [SRC_EXTRACT_DIR] [OUT_DIR]
#   SRC_EXTRACT_DIR  your AM2R 1.5.5 folder (data.win, lang/, mods/, the .ogg music)   [working/extracted/cu155]
#   OUT_DIR          where the SD tree is built                                     [dist/sd]
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${1:-$REPO/working/extracted/cu155}"
OUT="${2:-$REPO/dist/sd}"
DOL="$REPO/build/wii/boot.dol"
HBC="$REPO/wii/hbc"
APP="$OUT/apps/am2r"

die() { echo "sd-pack: $*" >&2; exit 1; }

[ -f "$SRC/data.win" ]           || die "no data.win in $SRC (point arg 1 at your AM2R 1.5.5 folder)"
[ -f "$SRC/lang/english.ini" ]   || die "no lang/english.ini in $SRC"
[ -d "$SRC/mods" ]               || die "no mods/ in $SRC: that is not a Community Update 1.5.5 folder"
[ -f "$DOL" ]                    || die "no $DOL — run 'make wii' first"

mkdir -p "$APP/game" "$APP/save"
cp -f "$DOL" "$APP/boot.dol"
for f in meta.xml icon.png; do [ ! -f "$HBC/$f" ] || cp -f "$HBC/$f" "$APP/$f"; done
rsync -a "$SRC/lang" "$SRC/mods" "$APP/game/"
for f in modifiers.ini explanations.txt; do [ ! -f "$SRC/$f" ] || cp -f "$SRC/$f" "$APP/game/"; done
cp -f "$SRC"/*.ogg "$APP/game/"
# The Wii's GPU binds nothing over 1024x1024: bigger texture pages are repacked losslessly and proven so.
rc=0
python3 "$REPO/tools/repack_textures.py" "$SRC/data.win" --check || rc=$?
case $rc in
0)
	python3 "$REPO/tools/repack_textures.py" "$SRC/data.win" "$APP/game/data.win"
	python3 "$REPO/tools/verify_repack.py" "$SRC/data.win" "$APP/game/data.win" | tail -n 1
	;;
3) rsync -a "$SRC/data.win" "$APP/game/data.win" ;;
*) die "repack_textures.py --check failed (exit $rc)" ;;
esac

echo "SD tree ready: $OUT"
echo "  copy its apps/ folder onto the SD card root, then launch AM2R GX from HBC."
( cd "$OUT" && find apps/am2r -maxdepth 2 -type f | sort | sed 's/^/    /' )
