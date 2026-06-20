#!/usr/bin/env bash
# Build the installable Speak Up archive from the canonical package/ source.
# Plain data-root .7z (NO fomod/ — a no-options FOMOD only triggers the buggy
# FOMOD Plus plugin; a plain archive installs cleanly in MO2/Vortex). 7-Zip is
# required (it writes spec-correct forward-slash entries; .NET zip writes
# backslashes and crashes MO2).
set -euo pipefail
VERSION="1.0.0"
PROJ="/c/Modding/VoiceSpellcasting"
PKG="$PROJ/package"
OUT="$PROJ/dist/SpeakUp-$VERSION.7z"
DL="/c/Modding/Mods/Skyrim/SpeakUp-$VERSION.7z"   # MO2 downloads (for install testing)
SEVENZ="/c/Program Files/7-Zip/7z.exe"; [ -x "$SEVENZ" ] || SEVENZ="/c/Modding/Modlists/Skyrim/Lorerim/tools/DIP/7-zip/7z.exe"

# Refresh the built DLL into the canonical package before zipping.
cp -f "$PROJ/build/Release/SpeakUp.dll" "$PKG/SKSE/Plugins/SpeakUp.dll"

rm -f "$OUT"
( cd "$PKG" && "$SEVENZ" a -t7z -mx=9 "$(cygpath -w "$OUT")" "*" >/dev/null )
cp -f "$OUT" "$DL"

# Write a MO2 download .meta so the archive appears in the Downloads tab (MO2 filters
# the list by gameName and hides files flagged removed/uninstalled). Without this,
# a manually-placed archive won't show up to install.
cat > "$DL.meta" <<EOF
[General]
gameName=skyrimse
modName=Speak Up
version=$VERSION
installed=false
uninstalled=false
removed=false
paused=false
EOF

echo "archive : $OUT"
echo "download: $DL (+ .meta)"
