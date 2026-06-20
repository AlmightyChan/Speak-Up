# Attribution

Speak Up is licensed GPL-3.0. It uses and/or studied the following:

- **Vosk** (speech recognition library + small-en-us model) — Apache-2.0.
  https://alphacephei.com/vosk/ . `libvosk.dll` 0.3.45 and the
  `vosk-model-small-en-us-0.15` model are redistributed under Apache-2.0. The mingw runtime
  DLLs (`libgcc_s_seh-1.dll`, `libstdc++-6.dll`, `libwinpthread-1.dll`) ship with the Vosk
  Windows release and are required by `libvosk.dll`.
- **nlohmann/json** (single-header JSON) — MIT. https://github.com/nlohmann/json
- **Dragonborn Speaks Naturally** — MIT. https://github.com/YihaoPeng/DragonbornSpeaksNaturally
  Studied for the plugin↔companion launch + named-pipe IPC pattern. No code copied verbatim.
- **CommonLibSSE-NG** — MIT. https://github.com/CharmedBaryon/CommonLibSSE-NG
- **Address Library for SKSE** — runtime dependency (user-installed).

The DBU-LoreRim commission's spell-classification / name-sanitization insight informed the
roster filtering and `SanitizeName`; no DBU code was used.
