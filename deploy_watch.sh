#!/bin/bash
SRC=/c/Modding/VoiceSpellcasting/build/Release/VoiceSpellcasting.dll
DST="/c/Modding/Modlists/Skyrim/Lorerim/mods/Voice Spellcasting/SKSE/Plugins/VoiceSpellcasting.dll"
for i in $(seq 1 480); do   # up to ~4 hours, every 30s
  if cp "$SRC" "$DST" 2>/dev/null; then
    echo "DEPLOYED Phase4 DLL at $(date +%H:%M:%S) (attempt $i)"; exit 0
  fi
  sleep 30
done
echo "deploy watcher timed out"; exit 1
