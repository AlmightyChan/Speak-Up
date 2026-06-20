# Speak Up

Cast, equip, and shout by **speaking the spell's name** — fully offline, plug-and-play,
and Windows 11-proof.

It reads your **live** spellbook every time you load a save and refreshes when you learn a
spell or transform (Vampire Lord / Werewolf), so it works with any modlist and any spells,
powers, or shouts you have — vanilla or modded, no patching, no database to maintain.
Recognition runs **in-process and offline** (Vosk/Kaldi), constrained to exactly what you
know, so it's fast, accurate, and nothing is ever sent anywhere.

## Requirements
- **SKSE64** and **Address Library for SKSE** (SE or AE).
- **MCM Helper** — *optional*, only for the in-game settings menu. The mod is fully
  functional without it (settings via INI).
- **powerofthree's Tweaks** — *recommended* (already in LoreRim); lets shouts recognize their
  Dovahzul words (e.g. "Fus Ro Dah").

## Install
Install with your mod manager (MO2/Vortex) and enable the mod **and** its plugin
(`SpeakUp.esp`, ESL-flagged — it costs no load-order slot). Launch normally; the recognizer
starts with the game and closes with it. There is **nothing to run or manage** yourself.

## How to speak commands
The phrase is the spell's **displayed name** (what you see in your Magic menu). Say it exactly.

| You say | Result |
|---|---|
| `firebolt` | **casts** Firebolt (right hand) |
| `left firebolt` / `right firebolt` | casts in that hand |
| `dual firebolt` | **dual-casts** — only if your perks allow it (auto-detected) |
| `equip firebolt` | equips to the right hand (you cast it yourself) |
| `equip left firebolt` / `equip dual firebolt` | equips left / both hands |
| `cast firebolt` / `cast dual firebolt` | explicit cast / dual cast |
| `conjure familiar` / `summon familiar` | casts (flavor synonyms) |
| `fus` / `fus ro` / `fus ro dah` | shouts that word level (Dovahzul) |
| `force` / `force balance` / `force balance push` | the same shout, in English |
| `<power name>` | uses a lesser/greater power |

- **Casting never equips** — a spellsword or fighter can cast by voice while keeping weapons
  in hand. Concentration and long-charge spells are equipped instead of instant-cast by
  default (toggleable).
- Shouts respect their per-word unlock state and cooldown (including Talos / cooldown gear).
- Dual-casting is gated by the game's own rules (your perks), so it stays correct on any
  modlist — vanilla, Requiem, or modded — with nothing to configure.

## Settings
Edit `SKSE/Plugins/SpeakUp.ini`, or use the in-game MCM (with MCM Helper). Highlights:
default action (cast vs equip), default equip hand, instant-cast behavior, recognition
sensitivity, a listen-toggle hotkey, and large-list scoping. Changes apply within a few
seconds — no restart needed.

## Antivirus note
The recognizer ships native libraries (`libvosk` + its runtime). Some antivirus tools flag
new unsigned DLLs heuristically. If voice does nothing, whitelist the mod's
`SKSE/Plugins/SpeakUp/` folder. It runs entirely offline — no network.

## Troubleshooting
- Plugin log: `Documents/My Games/Skyrim Special Edition/SKSE/SpeakUp.log`
  (LoreRim redirects this to `…/My Games/Skyrim.INI/SKSE/…`).
- Recognized speech is logged as `[rec] heard '<text>' (conf 0.NN)` for tuning.

## Credits / licenses
- Speech recognition: [Vosk](https://alphacephei.com/vosk/) (Apache-2.0).
- JSON: nlohmann/json (MIT). Architecture studied from Dragonborn Speaks Naturally (MIT).
- This mod: GPL-3.0.
