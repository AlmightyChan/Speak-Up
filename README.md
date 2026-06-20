# Speak Up

**Your voice is your power. So use it.**

Speak Up is an SKSE plugin for Skyrim SE/AE that lets you **cast and equip spells, powers,
and shouts — and run hands-free utilities** (open menus, wait, quicksave/load, and more) just
by speaking. Recognition runs **in-process and fully offline** via [Vosk](https://alphacephei.com/vosk/),
so it's fast, private, and needs no companion app.

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
| `firebolt` | cast it |
| `left firebolt` / `right firebolt` / `dual firebolt` | cast from that hand / dual-cast |
| `equip firebolt` | equip instead of cast |
| `fus` / `fus ro` / `fus ro dah` (or plain English) | shout by word level |
| `<power name>` | use a power |
| `open map`, `wait`, `quicksave`, `clear hands`, `toggle listening`, … | utilities |

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

### Fetching the Vosk runtime (not committed)

To keep the repo lean, the third-party Vosk binaries and model are **not** stored in git. To
build a runnable/packageable copy, place these under `package/SKSE/Plugins/SpeakUp/`:

- `libvosk.dll` + the MinGW runtime DLLs (`libgcc_s_seh-1.dll`, `libstdc++-6.dll`,
  `libwinpthread-1.dll`) — from the [Vosk 0.3.45 Windows release](https://github.com/alphacep/vosk-api/releases).
- `models/vosk-model-small-en-us-0.15/` — from the [Vosk model list](https://alphacephei.com/vosk/models).

Both are Apache-2.0 (see `package/LICENSES/`). `package_archive.sh` builds the installable
`.7z` from `package/`.

## Architecture (in short)

- **In-process, no companion / no IPC.** `libvosk.dll` is loaded at runtime; the recognizer
  lives in the plugin DLL. (An external companion was prototyped but dropped — WDAC/usvfs
  blocked an unsigned child exe.)
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
