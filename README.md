# Speak Up

**Your voice is your power. So use it.**

Speak Up is an SKSE plugin for Skyrim SE/AE that lets you **cast and equip spells, powers,
and shouts — and run hands-free utilities** (open menus, wait, quicksave/load, and more) just
by speaking. Recognition runs **in-process and fully offline** via
[sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) — an open-vocabulary streaming recognizer
that's phonetically fuzzy-matched to your live roster, so it handles fantasy proper nouns
(Atronach, Mehrunes Dagon, Tharn's Prison) that a fixed-grammar engine can't. Fast, private,
and needs no companion app.

It reads your **live spellbook** on every load and refreshes when you learn a spell or
transform (Vampire Lord / Werewolf), so it works with whatever you have — vanilla, DLC, or
modded — **with no patches and no spell database to maintain.** Truly plug-and-play.

> Speak Up is the first mod released as part of **Patchwork**, a toolkit for making Skyrim
> modding more accessible. It's a spiritual successor to *Dragonborn Unlimited* and
> *Dragonborn Speaks Naturally* — built clean-room on a modern framework. No code from either
> mod is used; both were inspirations only. See [ATTRIBUTION.md](ATTRIBUTION.md).

## How it works

| Say… | Result |
|------|--------|
| `Firebolt` | Casts Firebolt. |
| `Dual Cast Firebolt` | Dual-casts it, given the proper perks. |
| `Equip Firebolt Left` / `Right` / `Dual` | Equips the spell to the specified hand. With no hand it defaults to the left — configurable in the MCM. |
| `Fus` / `Fus Ro` / `Fus Ro Dah` (or `Force`, `Balance`, `Push`) | Casts the shout at that word level. Speak the Dovahzul if you know it; if not, read the words listed in the menu in plain English. Segmented casting — it only casts what you say. |
| `Open` / `Show` / `Close` / `Hide` + a menu name | Opens or closes that menu. |
| `Wait 6 hours` | Waits 6 hours, instantly. |

Casting plays as if you cast it yourself — magicka, perks, enchants, and silence all apply;
dual-cast is gated by your own perks on any magic overhaul.

## Requirements

- SKSE64
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
- [MCM Helper](https://www.nexusmods.com/skyrimspecialedition/mods/53000) (optional — for the in-game menu)
- powerofthree's Tweaks (recommended — for Dovahzul shout words)
- A microphone set as your Windows default input device

## Building from source

Requires the CommonLibSSE-NG toolchain (CMake ≥ 3.24, a C++23 MSVC, vcpkg).

```sh
cmake --preset vs2022-windows
cmake --build build --config Release
```

The build deploys `SpeakUp.dll` to the folder set by `VSC_DEPLOY_DIR` in `CMakePresets.json`
(edit it for your own MO2 mod path, or override with `-DVSC_DEPLOY_DIR=...`). Out-of-game unit
tests for the grammar/vocabulary live in `tests-cpp/`.

### Fetching the sherpa-onnx runtime (not committed)

To keep the repo lean, the third-party sherpa-onnx binaries and model are **not** stored in git.
To build a runnable/packageable copy, place these under `package/SKSE/Plugins/SpeakUp/`:

- `sherpa-onnx-c-api.dll` + `onnxruntime.dll` — from the
  [sherpa-onnx Windows x64 release](https://github.com/k2-fsa/sherpa-onnx/releases).
- `models/sherpa-onnx-streaming-zipformer-en-2023-06-26/` — the streaming Zipformer (int8)
  English model, from the [sherpa-onnx pre-trained models](https://github.com/k2-fsa/sherpa-onnx/releases/tag/asr-models).

Both are Apache-2.0 (see `package/LICENSES/`). `package_variants.sh` builds the installable
`SpeakUp-<version>.7z` from `package/` (single sherpa file).

## Architecture (in short)

- **In-process, no companion / no IPC.** `sherpa-onnx-c-api.dll` + `onnxruntime.dll` are loaded
  at runtime; the recognizer lives in the plugin DLL. The open-vocabulary transcript is matched
  to the live roster with a phonetic (Double Metaphone + edit-distance) fuzzy matcher. (An
  external companion was prototyped but dropped — WDAC/usvfs blocked an unsigned child exe.)
- **Live roster, no static data** — enumerates `addedSpells` + base/race spell lists via
  CommonLibSSE-NG and recognizes against a live grammar; refreshed on `SpellsLearned`,
  race-switch, and word-unlock events.
- **Crash-safe & save-safe** — no engine hooks, no co-save data, no script injection. Every
  game-object read/equip/cast is marshaled to the main thread via `SKSE::GetTaskInterface()`.
- **`UsesNoStructs(true)`** — one binary loads on both SE and AE (the trap DBU-era plugins
  fell into).

## License

GPL-3.0-or-later. See [LICENSE](LICENSE). Third-party components and their licenses are listed
in [ATTRIBUTION.md](ATTRIBUTION.md) and `package/LICENSES/`.
