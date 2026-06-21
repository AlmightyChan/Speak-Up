# Attribution

Speak Up is licensed GPL-3.0. It uses and/or studied the following:

- **sherpa-onnx** (speech recognition library + streaming Zipformer en model) — Apache-2.0.
  https://github.com/k2-fsa/sherpa-onnx . `sherpa-onnx-c-api.dll` and the
  `sherpa-onnx-streaming-zipformer-en-2023-06-26` model are redistributed under Apache-2.0.
- **onnxruntime** (inference runtime for the model) — MIT. Copyright (c) Microsoft.
  https://github.com/microsoft/onnxruntime . `onnxruntime.dll` redistributed unmodified.
- **nlohmann/json** (single-header JSON) — MIT. https://github.com/nlohmann/json
- **Dragonborn Speaks Naturally** — MIT. https://github.com/YihaoPeng/DragonbornSpeaksNaturally
  Studied for the plugin↔companion launch + named-pipe IPC pattern. No code copied verbatim.
- **CommonLibSSE-NG** — MIT. https://github.com/CharmedBaryon/CommonLibSSE-NG
- **Address Library for SKSE** — runtime dependency (user-installed).

The DBU-LoreRim commission's spell-classification / name-sanitization insight informed the
roster filtering and `SanitizeName`; no DBU code was used.
