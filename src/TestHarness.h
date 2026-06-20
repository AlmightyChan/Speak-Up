#pragma once

// ============================================================================
// TestHarness — dev/test instrumentation for Phase 1 (no game UI yet).
//
// INI-configurable hotkeys (optional modifier gate). Defaults — no modifier:
//   2 (top row): dump the live spell/power/shout roster to the log.
//   3 (top row): equip the NEXT roster entry (cycles) — proves equip across all
//                three categories without needing a known spell name.
// Remap via Data/SKSE/Plugins/VoiceSpellcasting.ini ([Debug] section).
//
// Compile-time toggle: VSC_ENABLE_TEST_HARNESS (PCH.h). Off in release builds.
// ============================================================================

#include "PCH.h"

#if VSC_ENABLE_TEST_HARNESS

namespace VSC
{
    // Register the keyboard input sink. Call at kDataLoaded.
    void InstallTestHarness();
}

#endif // VSC_ENABLE_TEST_HARNESS
