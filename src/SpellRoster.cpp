#include "PCH.h"
#include "SpellRoster.h"

namespace logger = SKSE::log;

namespace VSC
{
    namespace
    {
        // Map a SpellItem's engine spell-type to our spoken category, or nullopt
        // if it should NOT be in the spoken vocabulary (passive ability, the
        // per-word kVoicePower effect behind a shout, diseases/poisons, etc.).
        std::optional<Category> ClassifySpell(RE::SpellItem* a_spell)
        {
            using ST = RE::MagicSystem::SpellType;
            switch (a_spell->GetSpellType()) {
            case ST::kSpell:
                return Category::Spell;
            case ST::kPower:
            case ST::kLesserPower:
                return Category::Power;
            default:
                return std::nullopt;  // kAbility, kVoicePower, kDisease, ...
            }
        }

        std::string DisplayName(RE::TESForm* a_form)
        {
            if (auto* named = a_form->As<RE::TESFullName>()) {
                if (const char* n = named->GetFullName(); n && *n) {
                    return n;
                }
            }
            return {};
        }

        // Add every SpellItem in a TESSpellList::SpellData to the roster (deduped).
        void HarvestSpellData(RE::TESSpellList::SpellData* a_data,
                              std::unordered_set<RE::FormID>& a_seen,
                              std::vector<RosterEntry>& a_out)
        {
            if (!a_data || !a_data->spells) {
                return;
            }
            for (std::uint32_t i = 0; i < a_data->numSpells; ++i) {
                auto* spell = a_data->spells[i];
                if (!spell) {
                    continue;
                }
                if (!a_seen.insert(spell->GetFormID()).second) {
                    continue;  // already harvested
                }
                auto category = ClassifySpell(spell);
                if (!category) {
                    continue;
                }
                std::string name = DisplayName(spell);
                if (name.empty()) {
                    continue;  // unnamed -> unspeakable
                }
                a_out.push_back({ spell, *category, std::move(name), spell->GetFormID() });
            }
        }
    }

    std::vector<RosterEntry> BuildRoster()
    {
        std::vector<RosterEntry> roster;
        std::unordered_set<RE::FormID> seen;

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            logger::error("[roster] no player singleton");
            return roster;
        }

        // 1) Learned spells/powers (the uncapped live list).
        for (auto* spell : player->GetActorRuntimeData().addedSpells) {
            if (!spell) {
                continue;
            }
            if (!seen.insert(spell->GetFormID()).second) {
                continue;
            }
            auto category = ClassifySpell(spell);
            if (!category) {
                continue;
            }
            std::string name = DisplayName(spell);
            if (name.empty()) {
                continue;
            }
            roster.push_back({ spell, *category, std::move(name), spell->GetFormID() });
        }

        // 2) Base-NPC default/quest-granted spell list.
        if (auto* base = player->GetActorBase()) {
            HarvestSpellData(base->GetSpellList(), seen, roster);
        }

        // 3) Racial powers/abilities.
        if (auto* race = player->GetRace()) {
            HarvestSpellData(race->actorEffects, seen, roster);
        }

        // 4) Known shouts (global form-flag GetKnown(); per-word level refined later).
        if (auto* dataHandler = RE::TESDataHandler::GetSingleton()) {
            for (auto* shout : dataHandler->GetFormArray<RE::TESShout>()) {
                if (!shout || !shout->GetKnown()) {
                    continue;
                }
                std::string name = DisplayName(shout);
                if (name.empty()) {
                    continue;
                }
                roster.push_back({ shout, Category::Shout, std::move(name), shout->GetFormID() });
            }
        }

        logger::info("[roster] built: {} entries", roster.size());
        return roster;
    }
}
