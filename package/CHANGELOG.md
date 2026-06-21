# Changelog

## 1.2.1 — shout unification, "Fus Ro Dah" fix, MCM Voice page, logging
- **Saying a shout's NAME now matches saying its WORDS.** "Unrelenting Force" triggers the shout
  at your highest known word level — the exact same result as "Fus Ro Dah" or "Force Balance Push".
  Previously the name hand-cast a same-named spell with no cooldown and no thu'um, because a spell
  sharing the shout's display name claimed the spoken phrase first in the grammar. The shout now
  owns its own cast-by-name phrases, so all three entry points share one cast pathway.
- **"Fus Ro Dah" and other Words of Power are now heard correctly.** Word-of-Power names store
  vowel sounds as a dragon-font cipher (1=aa, 2=ei, 3=ii, 4=ah, 8=oo); these are decoded so e.g.
  word 3 of Unrelenting Force ("D4" → "Dah") is recognized instead of falling back to English.
- **The MCM "Voice" page no longer renders empty.** Removed unsupported keymap controls and
  undocumented group properties that caused MCM Helper to silently fail drawing the page.
  Push-to-talk and listen-toggle keys are configured in `SKSE/Plugins/SpeakUp.ini`.
- **Explicit per-command logging.** Every recognized phrase now logs what was heard, the command
  it matched, and a single outcome — cast (with magicka cost), blocked (with reason), on-cooldown,
  equipped-instead, or unknown. No recognized command can silently do nothing, so bug reports are
  precise.
- **sherpa-onnx is now the sole shipped engine.** The Vosk small/lgraph variants were retired; the
  release ships as a single file (`SpeakUp-<version>.7z`).

## 1.2.0 — sherpa-onnx engine + lifecycle fixes
- Added **sherpa-onnx** as a second recognition engine (Engine=1 in INI/MCM): open-vocabulary
  ASR + Jaro-Winkler fuzzy match against the live spell roster. Handles proper nouns (Atronach,
  Mehrunes Dagon, etc.) that grammar-constrained Vosk cannot hear, at the cost of a larger model.
- Three distribution variants (small / lgraph / sherpa) each ship only their own engine's runtime
  and model — clean, lean installs with no unused 200 MB model sitting on disk.
- Fixed plugin load failure for Windows users whose profile path (Documents/…) contains non-ASCII
  characters: `SetupLog` now builds the spdlog rotating sink via `wstring` to avoid the narrow-
  string conversion throw, with a `%TEMP%\SpeakUp.log` fallback if that also fails.
- Version unified: CMake `project(VERSION …)` is now the single source of truth; the SKSE
  `PluginVersion` struct, vcpkg.json, and the packaging script all derive from it automatically.
- Removed dead `recognizer_set_grm` symbol from VoskLoader (typedef + member + GetProcAddress);
  it was never called and left a misleading comment implying grammar hot-swap was implemented.
- Fixed `SherpaRecognizer.cpp` stale comment referencing the deleted `sherpa_spike.cpp` prototype.
- `ShoutUseRealCast` and `ShoutKeyDX` are now visible on the MCM **Advanced** page (toggle +
  keymap) in addition to the INI, so players on exotic keyboard layouts or exclusive-fullscreen
  mode can adjust them in-game without editing files.
- Packaging script (`package_variants.sh`) now derives VERSION by grepping CMakeLists.txt,
  fixes silent file-drop for paths with spaces (`find -print0 | while IFS= read -r -d ''`),
  and verifies each archive contains SpeakUp.dll/.esp/.ini, MCM config.json, and the model
  directory before copying to Downloads — failing loudly on any missing file.

## 0.1.0 — Initial release
- Cast, equip, and shout by voice from your live spell/power/shout roster.
- Fully offline, in-process recognition (Vosk); plug-and-play, no per-mod patches.
- Hand selection (left/right/dual); dual-casting gated by the engine's own perk rules,
  with the spell's actual dual-cast magicka scale.
- Stance-based cast origin: from the head when weapons are sheathed, from the hand when drawn.
- Cast-without-equip (spellsword friendly); concentration/long-charge spells equip by default.
- Per-word shouts in Dovahzul (with g2p-friendly re-spellings) or English; per-word unlock +
  cooldown (incl. ShoutRecoveryMult).
- Voice utility commands: open/close menus, "wait N hours", quicksave/load, clear hands/voice,
  start/stop listening — all keybind-free (direct engine calls, no key simulation).
- Push-to-talk and listen-toggle, each with an optional modifier, bound via the MCM.
- In-game MCM (MCM Helper) + INI configuration; near-live setting changes.
- Developer tab: optional debug logging and grammar dump (off by default).
- Crash-safe and save-safe (no engine hooks, no co-save, no script injection); ESL-flagged plugin.
