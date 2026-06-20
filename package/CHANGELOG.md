# Changelog

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
