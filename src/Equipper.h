#pragma once

// ============================================================================
// Equipper / action executor. ALL engine mutation is marshaled to the main thread
// via SKSE::GetTaskInterface()->AddTask (the #1 SKSE crash cause is touching game
// objects off-thread; recognition results arrive on the pipe thread).
//
//   Equip: ActorEquipManager::EquipSpell (right/left/both) / EquipShout / power->voice
//   Cast : MagicCaster::CastSpellImmediate from the chosen hand (auto-cast);
//          dual = ActorMagicCaster::SetDualCasting(true) for an overcharged cast;
//          shout = DUAL PATH: real engine pipeline (equip + SendInput) for shouts
//                  whose effects need it; CastSpellImmediate for simple shouts.
// ============================================================================

#include "PCH.h"
#include "Types.h"
#include "SpellRoster.h"
#include <atomic>

namespace VSC
{
    // Casting behavior settings (from INI / MCM). Set on the main thread at config load.
    struct CastSettings
    {
        bool          instantCast = true;          // voice "cast" instant-casts; else it equips
        bool          allowConcentration = false;  // allow instant-casting concentration spells
        bool          allowLongCast = false;       // allow instant-casting long-charge spells
        float         longCastThreshold = 1.0f;    // seconds of charge time that counts as "long"
        Hand          equipHand = Hand::Left;      // hand used when a cast falls back to equip
        bool          playShoutAnimation = false;  // play the shout body animation on a voice-cast shout
        bool          shoutUseRealCast = true;     // 1 = dual-path auto-detect; 0 = force legacy CastSpellImmediate
        bool          shoutPlayVoice = false;      // 1 = the character vocalizes the Thu'um (real pipeline for ALL
                                                   // shouts); 0 (default, immersion) = silent — you are the voice, so
                                                   // shouts fire their effect directly; only shouts that REQUIRE the
                                                   // engine pipeline (movement/script/etc.) still run it.
        std::uint32_t shoutKeyDX = 0x39;           // DX scan code for the Shout/Sheathe key (default 0x39 = Space).
                                                   // Read from ControlMap at cast time; this is the INI fallback.
        bool          shoutRestoreEquipped = true; // 1 (default): a voice-cast shout that runs the pipeline restores
                                                   // whatever shout/power you had equipped afterwards (the pipeline
                                                   // equips the spoken shout to fire it; this puts your original back).
        bool          shoutInstantPipeline = true; // 1 (default): instead of HOLDING the Shout key 0.2–1.2s to charge
                                                   // the word level, briefly collapse the engine's word-level
                                                   // thresholds (fShoutTime1/2) around a ~one-frame key tap so the
                                                   // chosen word fires almost instantly — still via the full native
                                                   // pipeline, so movement/script effects fire. 0 = the legacy timed
                                                   // hold (safe fallback if a setup mis-resolves the word level).
        // (The per-word-level key HOLD durations are derived live from the engine's
        // fShoutTime1/fShoutTime2 game settings at cast time — see Equipper.cpp.)
    };
    void SetCastSettings(const CastSettings& a_settings);

    // Atomic guard: true while CastShoutNow is injecting the Shout key via SendInput.
    // ListenHotkeySink in VoiceController checks this and skips PTT/toggle processing
    // for that synthetic key event so it does not toggle push-to-talk listening.
    // Written on the main thread (SKSE task); read on the input-event thread.
    extern std::atomic<bool> g_injectingShoutKey;

    // Execute an action on the player. Safe to call from any thread.
    // a_shoutLevel: for shout casts, the word level (0/1/2) to cast; -1 = highest unlocked.
    // a_stanceOrigin: bare cast — spawn from the head when weapons are sheathed, from the
    //   right hand when drawn (immersive default; ignored for explicit hand / dual casts).
    void Execute(const RosterEntry& a_entry, Action a_action, Hand a_hand, bool a_dual,
                 int a_shoutLevel = -1, bool a_stanceOrigin = false);

    // Convenience: equip to the given hand (used by the debug hotkey).
    void EquipEntry(const RosterEntry& a_entry, Hand a_hand = Hand::Right);
}
