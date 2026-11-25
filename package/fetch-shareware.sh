#!/bin/sh
# Optional: download id Software's freely distributable DOOM shareware v1.9 (Episode 1, "Knee-Deep in the Dead")
# from the idgames archive and verify it before use. Nothing is downloaded unless you run this (or say yes in install.sh).
# The shareware release may be freely redistributed unmodified; it is NOT the full game.
set -e
DEST="${1:-$HOME/.local/share/lapis-doom/iwads}"
WAD_MD5=f0cefca49926d00903cf57551d901abe
WAD_SHA256=1d7d43be501e67d927e415e0b8f3e29c3bf33075e859721816f652a526cac771
ZIP_SHA256=cacf0142b31ca1af00796b4a0339e07992ac5f21bc3f81e7532fe1b5e1b486e6
MIRRORS="https://youfailit.net/pub/idgames https://www.quaddicted.com/files/idgames https://ftpmirror1.infania.net/pub/idgames"
mkdir -p "$DEST"; TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
for m in $MIRRORS; do
  echo "Downloading doom19s.zip from $m ..."
  if curl -fsSL --max-time 120 -o "$TMP/doom19s.zip" "$m/idstuff/doom/doom19s.zip"; then
    if echo "$ZIP_SHA256  $TMP/doom19s.zip" | sha256sum -c --status; then break; fi
    echo "  checksum mismatch, trying next mirror"; rm -f "$TMP/doom19s.zip"
  fi
done
[ -f "$TMP/doom19s.zip" ] || { echo "Could not download a verified doom19s.zip"; exit 1; }
python3 - "$TMP/doom19s.zip" "$DEST/DOOM1.WAD.tmp" <<'PY'
import io, sys, zipfile
outer = zipfile.ZipFile(sys.argv[1])
blob = outer.read("DOOMS_19.1") + outer.read("DOOMS_19.2")   # split PKLITE self-extracting zip
open(sys.argv[2], "wb").write(zipfile.ZipFile(io.BytesIO(blob)).read("DOOM1.WAD"))
PY
if echo "$WAD_MD5  $DEST/DOOM1.WAD.tmp" | md5sum -c --status && echo "$WAD_SHA256  $DEST/DOOM1.WAD.tmp" | sha256sum -c --status; then
  mv "$DEST/DOOM1.WAD.tmp" "$DEST/DOOM1.WAD"; echo "Verified shareware DOOM1.WAD v1.9 -> $DEST/DOOM1.WAD"
else
  rm -f "$DEST/DOOM1.WAD.tmp"; echo "DOOM1.WAD checksum mismatch; not installed"; exit 1
fi
