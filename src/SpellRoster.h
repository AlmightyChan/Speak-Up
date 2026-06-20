#pragma once

// ============================================================================
// SpellRoster — reads the player's LIVE castable vocabulary directly from engine
// form pointers. This is the heart of the DBU replacement: no static INI, no
// Papyrus counter, no ~43-spell cap. Whatever the player actually knows in the
// current load order is what we enumerate.
//
// Sources (deduped by FormID):
//   - Actor::GetActorRuntimeData().addedSpells  (learned spells/powers — no cap)
//   - player base TESNPC::GetSpellList()        (default/quest-granted)
//   - player TESRace actorEffects               (racial powers/abilities)
//   - TESDataHandler GetFormArray<TESShout>() filtered by GetKnown() (shouts)
//
// Classification (RE::MagicSystem::SpellType, verified against headers):
//   kSpell(0)                         -> Spell
//   kPower(2), kLesserPower(3)        -> Power
//   everything else (kAbility passive, kVoicePower per-word shout effects,
//   kDisease/kPoison/kEnchantment/...) -> excluded from the spoken vocabulary.
// ============================================================================

#include "PCH.h"
#include "Types.h"

namespace VSC
{
    struct RosterEntry
    {
        RE::TESForm* form;      // SpellItem* (Spell/Power) or TESShout* (Shout)
        Category     category;
        std::string  name;      // display name (TESFullName::GetFullName)
        RE::FormID   formID;    // runtime FormID (for logging / phrase->form map)
    };

    // Build the player's live castable roster. MUST be called on the main thread
    // (marshal via SKSE::GetTaskInterface()->AddTask if calling off-thread).
    std::vector<RosterEntry> BuildRoster();
}
