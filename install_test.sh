#!/usr/bin/env bash
# install_test.sh — install the CANONICAL built artifact (dist/SpeakUp-<version>.7z)
# into the LoreRim MO2 mods folder so the test playground runs the EXACT file we ship
# to GitHub/Nexus — not a piecemeal dev assembly.
#
# Separation of environments:
#   * PROJECT (dist/)  = development + the one true canonical artifact.
#   * MO2 mods folder  = a disposable playground used only to PLAY/TEST. We push the
#                        canonical artifact into it here; we never treat it as the source.
#
# Run package_variants.sh first to (re)build dist/SpeakUp-<version>.7z, then run this.
set -euo pipefail

PROJ="/c/BASECAMP/projects/Thane/mods/SpeakUp"
DIST="$PROJ/dist"
MOD_DIR="/c/Modding/Modlists/Skyrim/Lorerim/mods/Speak Up (sherpa)"
SEVENZ="/c/Program Files/7-Zip/7z.exe"
[ -x "$SEVENZ" ] || SEVENZ="/c/Modding/Modlists/Skyrim/Lorerim/tools/DIP/7-zip/7z.exe"

VERSION=$(grep -m1 '^ *VERSION [0-9]' "$PROJ/CMakeLists.txt" | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
ARCHIVE="$DIST/SpeakUp-$VERSION.7z"
[ -f "$ARCHIVE" ] || { echo "ERROR: $ARCHIVE not found — run package_variants.sh first." >&2; exit 1; }

echo "Installing canonical $ARCHIVE"
echo "  -> $MOD_DIR  (preserving MO2 meta.ini)"

# Clear SpeakUp's own payload (this mod folder contains ONLY SpeakUp files), then extract
# the canonical archive over it. MO2's meta.ini at the mod root is left untouched.
rm -rf "$MOD_DIR/SKSE" "$MOD_DIR/MCM"
rm -f  "$MOD_DIR/SpeakUp.esp"
mkdir -p "$MOD_DIR"
"$SEVENZ" x -y -o"$(cygpath -w "$MOD_DIR")" "$(cygpath -w "$ARCHIVE")" >/dev/null

echo "done — test playground now runs the exact v$VERSION release file."
