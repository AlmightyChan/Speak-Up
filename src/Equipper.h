#pragma once

// ============================================================================
// Equipper / action executor. ALL engine mutation is marshaled to the main thread
// via SKSE::GetTaskInterface()->AddTask (the #1 SKSE crash cause is touching game
// objects off-thread; recognition results arrive on the pipe thread).
//
//   Equip: ActorEquipManager::EquipSpell (right/left/both) / EquipShout / power->voice
//   Cast : MagicCaster::CastSpellImmediate from the chosen hand (auto-cast);
//          dual = ActorMagicCaster::SetDualCasting(true) for an overcharged cast;
//          shout = cast the shout's first-word variation spell.
// ============================================================================

#include "PCH.h"
#include "Types.h"
#include "SpellRoster.h"

namespace VSC
{
    // Casting behavior settings (from INI / MCM). Set on the main thread at config load.
    struct CastSettings
    {
        bool  instantCast = true;          // voice "cast" instant-casts; else it equips
        bool  allowConcentration = false;  // allow instant-casting concentration spells
        bool  allowLongCast = false;       // allow instant-casting long-charge spells
        float longCastThreshold = 1.0f;    // seconds of charge time that counts as "long"
        Hand  equipHand = Hand::Left;      // hand used when a cast falls back to equip
        bool  playShoutAnimation = false;  // play the shout body animation on a voice-cast shout
    };
    void SetCastSettings(const CastSettings& a_settings);

    // Execute an action on the player. Safe to call from any thread.
    // a_shoutLevel: for shout casts, the word level (0/1/2) to cast; -1 = highest unlocked.
    // a_stanceOrigin: bare cast — spawn from the head when weapons are sheathed, from the
    //   right hand when drawn (immersive default; ignored for explicit hand / dual casts).
    void Execute(const RosterEntry& a_entry, Action a_action, Hand a_hand, bool a_dual,
                 int a_shoutLevel = -1, bool a_stanceOrigin = false);

    // Convenience: equip to the given hand (used by the debug hotkey).
    void EquipEntry(const RosterEntry& a_entry, Hand a_hand = Hand::Right);
}
