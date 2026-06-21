#!/usr/bin/env bash
# Build the single, lean Speak Up install for release:
#   SpeakUp-<VERSION>.7z  — sherpa-onnx engine (Engine=1)
# sherpa is the SOLE shipped engine (the Vosk small/lgraph variants were retired).
# It ships ONLY its own engine's runtime + model, with the engine fixed in the
# shipped SpeakUp.ini. Plain data-root .7z (forward slashes).
#
# VERSION is derived from the project() VERSION line in CMakeLists.txt so there is
# a single source of truth.  Do NOT hardcode it here.
set -euo pipefail

PROJ="/c/BASECAMP/projects/Thane/mods/SpeakUp"
PKG="$PROJ/package"
SP="$PKG/SKSE/Plugins/SpeakUp"   # runtime + models live here
# CANONICAL output lives in the PROJECT, not the play/mods folder. dist/ holds the
# one true SpeakUp-<version>.7z — the exact file uploaded to GitHub/Nexus. The MO2
# mods folder is a disposable test playground; install into it FROM dist/ when testing.
DIST="$PROJ/dist"
SEVENZ="/c/Program Files/7-Zip/7z.exe"
[ -x "$SEVENZ" ] || SEVENZ="/c/Modding/Modlists/Skyrim/Lorerim/tools/DIP/7-zip/7z.exe"

# --- Derive version from CMakeLists.txt (single source of truth) ---
VERSION=$(grep -m1 '^ *VERSION [0-9]' "$PROJ/CMakeLists.txt" | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
if [ -z "$VERSION" ]; then
    echo "ERROR: could not parse VERSION from $PROJ/CMakeLists.txt" >&2
    exit 1
fi
echo "=== building Speak Up (sherpa) v$VERSION ==="

mkdir -p "$DIST"

# Refresh the built DLL into the package.
cp -f "$PROJ/build/Release/SpeakUp.dll" "$PKG/SKSE/Plugins/SpeakUp.dll"

# ---------------------------------------------------------------------------
# verify_archive <archive> <tag>
#   Checks that the .7z contains the mandatory files; exits non-zero if any
#   are missing so the build fails loudly rather than shipping a broken archive.
# ---------------------------------------------------------------------------
verify_archive() {
    local archive="$1" tag="$2"
    local missing=0
    local required=(
        "SKSE/Plugins/SpeakUp.dll"
        "SpeakUp.esp"
        "SKSE/Plugins/SpeakUp.ini"
        "MCM/Config/SpeakUp/config.json"
    )
    # List archive contents once (normalize backslashes to forward slashes for grep).
    local contents
    contents=$("$SEVENZ" l -ba -slt "$(cygpath -w "$archive")" 2>/dev/null | grep '^Path = ' | sed 's/^Path = //' | tr '\\' '/')

    for req in "${required[@]}"; do
        if ! echo "$contents" | grep -qF "$req"; then
            echo "  ERROR [$tag]: archive missing required file: $req" >&2
            missing=1
        fi
    done

    # Check that the variant's model directory is present.
    if ! echo "$contents" | grep -qE 'SKSE/Plugins/SpeakUp/(models/|sherpa-onnx)'; then
        echo "  ERROR [$tag]: archive missing model/runtime directory under SKSE/Plugins/SpeakUp/" >&2
        missing=1
    fi

    if [ "$missing" -ne 0 ]; then
        echo "  ABORT: $archive is incomplete — NOT copying to Downloads." >&2
        exit 1
    fi
    echo "  verify OK: $tag"
}

build_variant() {
    local tag="$1" engine="$2"; shift 2; local keep=("$@")
    local stage="$PROJ/tmp/pkg-$tag"
    rm -rf "$stage"; mkdir -p "$stage"

    # 1) Copy the common payload (everything EXCEPT the big SpeakUp/ runtime+model dir).
    #    Use -print0 / read -r -d '' so paths with spaces are handled correctly.
    ( cd "$PKG" && find . -path ./SKSE/Plugins/SpeakUp -prune -o -type f -print0 ) |
    while IFS= read -r -d '' f; do
        mkdir -p "$stage/$(dirname "$f")"
        cp -f "$PKG/$f" "$stage/$f"
    done

    # 2) Fix the engine in the shipped SKSE INI for this variant.
    sed -i "s/^Engine=.*/Engine=$engine/" "$stage/SKSE/Plugins/SpeakUp.ini"

    # 3) Copy only this variant's runtime DLLs + model.
    for k in "${keep[@]}"; do
        mkdir -p "$stage/SKSE/Plugins/SpeakUp/$(dirname "$k")"
        cp -r "$SP/$k" "$stage/SKSE/Plugins/SpeakUp/$k"
    done

    # 4) Zip. sherpa is the sole shipped file, so the archive is untagged.
    local out="$DIST/SpeakUp-$VERSION.7z"; rm -f "$out"
    ( cd "$stage" && "$SEVENZ" a -t7z -mx=9 "$(cygpath -w "$out")" "*" >/dev/null )

    # 5) Verify the archive is complete. The canonical artifact stays in dist/ (project).
    verify_archive "$out" "$tag"

    echo "  $tag -> $out  ($(du -h "$out" | cut -f1))"
    rm -rf "$stage"
}

build_variant sherpa 1 sherpa-onnx-c-api.dll onnxruntime.dll models/sherpa-onnx-streaming-zipformer-en-2023-06-26

echo "=== done. Canonical artifact (upload THIS to GitHub/Nexus): ==="
ls -la "$DIST"/SpeakUp-"$VERSION".7z | awk '{print $5, $NF}'
