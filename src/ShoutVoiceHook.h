#pragma once

// ============================================================================
// ShoutVoiceHook — optionally suppress the player's Thu'um shout VOCALIZATION
// (the dragon-language voice line the character yells) while leaving the shout's
// effect, movement, animation, and cooldown completely intact. Used for the
// "silent shouts" immersion mode (ShoutPlayVoice=OFF): you spoke the words, so
// you are the voice.
//
// It hooks the engine's call to the RE::DialogueItem constructor (which builds
// every spoken dialogue line) and cancels ONLY the player's VoicePower (shout)
// lines. The hook is the call-site patch proven by the production mod
// ConsiderateFollowers (Address Library id 25541 + 0xE2, AE).
//
// Robustness: the verified call-site id is AE-specific, so the hook installs on
// AE runtimes only and NO-OPS everywhere else (SE/VR) — the mod stays fully
// functional, shouts simply vocalize. It is also fail-safe on AE: it verifies the
// expected CALL opcode is present before patching and skips if anything is off, so
// it never hooks a wrong address or crashes an unknown game update / load order.
// ============================================================================

namespace VSC
{
    // Install the hook. Call once after SKSE init (e.g. kDataLoaded). No-ops
    // safely if the engine address can't be resolved on this runtime.
    void InstallShoutVoiceMuteHook();

    // Toggle suppression at runtime. true = drop the player's shout voice lines
    // (silent mode); false = let them play. Wired to the ShoutPlayVoice setting.
    void SetShoutVoiceMuted(bool a_muted);
}
