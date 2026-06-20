#pragma once

// ============================================================================
// Precompiled header for Voice Spellcasting.
// Included automatically by CMake target_precompile_headers — do not #include
// it manually in .cpp files.
// ============================================================================

// ----------------------------------------------------------------------------
// Compile-time feature toggles
// ----------------------------------------------------------------------------

// VSC_ENABLE_TEST_HARNESS — enables dev/test instrumentation: the debug hotkey
// input sink that dumps the live spell roster and equips a chosen form. Set to 0
// before shipping to players (no stubs / dev paths in the release build).
#define VSC_ENABLE_TEST_HARNESS 0

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <memory>
#include <functional>
#include <algorithm>
#include <format>
#include <filesystem>

using namespace std::literals;

// CommonLibSSE-NG (pulls in all RE:: and SKSE:: headers + bundled spdlog)
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
