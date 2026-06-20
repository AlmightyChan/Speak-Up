#include "PCH.h"
#include "GlobalCommands.h"

#include <cmath>

namespace logger = SKSE::log;

namespace VSC
{
    const std::unordered_map<std::string, GlobalAction>& GlobalCommandPhrases()
    {
        // Phrases are pre-normalized (lower-case, single spaces) to match recognizer output.
        static const std::unordered_map<std::string, GlobalAction> map = {
            { "start listening", GlobalAction::StartListening },
            { "resume listening", GlobalAction::StartListening },
            { "stop listening",  GlobalAction::StopListening },
            { "pause listening", GlobalAction::StopListening },

            { "clear hands",     GlobalAction::ClearHands },
            { "empty hands",     GlobalAction::ClearHands },
            { "clear voice",     GlobalAction::ClearVoice },
            { "clear shout",     GlobalAction::ClearVoice },
            { "clear power",     GlobalAction::ClearVoice },

            { "wait",            GlobalAction::Wait },
            { "open wait menu",  GlobalAction::Wait },

            { "quick save",      GlobalAction::QuickSave },
            { "save game",       GlobalAction::QuickSave },
            { "quick load",      GlobalAction::QuickLoad },

            // Open ("open"/"show" <menu>)
            { "open map",        GlobalAction::OpenMap },       { "show map",       GlobalAction::OpenMap },
            { "open inventory",  GlobalAction::OpenInventory }, { "show inventory", GlobalAction::OpenInventory },
            { "open magic",      GlobalAction::OpenMagic },     { "show magic",     GlobalAction::OpenMagic },
            { "open spells",     GlobalAction::OpenMagic },     { "show spells",    GlobalAction::OpenMagic },
            { "open skills",     GlobalAction::OpenSkills },    { "show skills",    GlobalAction::OpenSkills },
            { "open journal",    GlobalAction::OpenJournal },   { "show journal",   GlobalAction::OpenJournal },

            // Close ("close"/"hide <menu>") — by name, mirroring the open verbs.
            { "close map",       GlobalAction::CloseMap },       { "hide map",       GlobalAction::CloseMap },
            { "close inventory", GlobalAction::CloseInventory }, { "hide inventory", GlobalAction::CloseInventory },
            { "close magic",     GlobalAction::CloseMagic },     { "hide magic",     GlobalAction::CloseMagic },
            { "close spells",    GlobalAction::CloseMagic },     { "hide spells",    GlobalAction::CloseMagic },
            { "close skills",    GlobalAction::CloseSkills },    { "hide skills",    GlobalAction::CloseSkills },
            { "close journal",   GlobalAction::CloseJournal },   { "hide journal",   GlobalAction::CloseJournal },

            // Fallback: close whatever menu is open.
            { "close menu",      GlobalAction::CloseMenu },     { "hide menu",      GlobalAction::CloseMenu },
            { "close",           GlobalAction::CloseMenu },     { "go back",        GlobalAction::CloseMenu },
            { "close all menus", GlobalAction::CloseMenu },     { "exit menu",      GlobalAction::CloseMenu },
        };
        return map;
    }

    namespace
    {
        RE::BGSEquipSlot* HandSlot(RE::DEFAULT_OBJECT a_slot)
        {
            auto* dom = RE::BGSDefaultObjectManager::GetSingleton();
            return dom ? dom->GetObject<RE::BGSEquipSlot>(a_slot) : nullptr;
        }

        void ShowMenu(std::string_view a_menu)
        {
            if (auto* q = RE::UIMessageQueue::GetSingleton()) {
                q->AddMessage(a_menu, RE::UI_MESSAGE_TYPE::kShow, nullptr);
            }
        }

        // Close a SPECIFIC menu by name (only if it's actually open).
        void HideMenu(std::string_view a_menu)
        {
            auto* ui = RE::UI::GetSingleton();
            auto* q  = RE::UIMessageQueue::GetSingleton();
            if (ui && q && ui->IsMenuOpen(a_menu)) {
                q->AddMessage(a_menu, RE::UI_MESSAGE_TYPE::kHide, nullptr);
            }
        }

        // Close whatever top-level menu is open by sending kHide to each candidate that
        // the UI reports as open. (We can open these by voice but previously couldn't
        // close them — keyboard/gamepad close is keybind-driven; this is keybind-free.)
        void CloseOpenMenus()
        {
            auto* ui = RE::UI::GetSingleton();
            auto* q  = RE::UIMessageQueue::GetSingleton();
            if (!ui || !q) return;
            static const std::string_view kMenus[] = {
                RE::JournalMenu::MENU_NAME, RE::MapMenu::MENU_NAME, RE::InventoryMenu::MENU_NAME,
                RE::MagicMenu::MENU_NAME, RE::StatsMenu::MENU_NAME, RE::SleepWaitMenu::MENU_NAME,
                RE::TweenMenu::MENU_NAME, RE::FavoritesMenu::MENU_NAME, RE::ContainerMenu::MENU_NAME,
                RE::BarterMenu::MENU_NAME, RE::GiftMenu::MENU_NAME, RE::BookMenu::MENU_NAME,
            };
            int closed = 0;
            for (auto name : kMenus) {
                if (ui->IsMenuOpen(name)) { q->AddMessage(name, RE::UI_MESSAGE_TYPE::kHide, nullptr); ++closed; }
            }
            logger::info("[global] close menu ({} closed)", closed);
        }

        // Unequip whatever spell is in the given hand (spells are TESBoundObjects).
        void ClearHand(RE::ActorEquipManager* a_eqm, RE::PlayerCharacter* a_player, bool a_leftHand)
        {
            auto* obj = a_player->GetEquippedObject(a_leftHand);
            if (!obj) return;
            if (auto* spell = obj->As<RE::SpellItem>()) {
                auto slot = HandSlot(a_leftHand ? RE::DEFAULT_OBJECT::kLeftHandEquip
                                                : RE::DEFAULT_OBJECT::kRightHandEquip);
                a_eqm->UnequipObject(a_player, spell, nullptr, 1, slot);
            }
        }

        void DoOnMainThread(std::function<void(RE::PlayerCharacter*)> a_fn)
        {
            auto* task = SKSE::GetTaskInterface();
            if (!task) return;
            task->AddTask([a_fn = std::move(a_fn)]() {
                if (auto* player = RE::PlayerCharacter::GetSingleton()) a_fn(player);
            });
        }
    }

    void RunGlobalAction(GlobalAction a_action)
    {
        // ALL of these touch engine/UI objects, so the WHOLE action is marshaled to the
        // main thread here — RunGlobalAction is invoked from the recognizer (mic) thread,
        // and off-main-thread engine access is the #1 SKSE crash cause. (None of these
        // simulate key presses; they call the engine directly, so they are keybind-free.)
        DoOnMainThread([a_action](RE::PlayerCharacter* p) {
            switch (a_action) {
            case GlobalAction::ClearHands:
                if (auto* eqm = RE::ActorEquipManager::GetSingleton()) {
                    ClearHand(eqm, p, false);  // right
                    ClearHand(eqm, p, true);   // left
                    logger::info("[global] clear hands");
                }
                break;
            case GlobalAction::ClearVoice: {
                auto* eqm = RE::ActorEquipManager::GetSingleton();
                auto* obj = p->GetEquippedObject(false);  // voice powers report here in some cases
                if (eqm && obj) {
                    if (auto* spell = obj->As<RE::SpellItem>(); spell && spell->GetSpellType() != RE::MagicSystem::SpellType::kSpell) {
                        auto* dom = RE::BGSDefaultObjectManager::GetSingleton();
                        auto* voice = dom ? dom->GetObject<RE::BGSEquipSlot>(RE::DEFAULT_OBJECT::kVoiceEquip) : nullptr;
                        eqm->UnequipObject(p, spell, nullptr, 1, voice);
                    }
                }
                logger::info("[global] clear voice");
                break;
            }
            case GlobalAction::Wait:          ShowMenu(RE::SleepWaitMenu::MENU_NAME); logger::info("[global] wait"); break;
            case GlobalAction::CloseMenu:     CloseOpenMenus(); break;
            case GlobalAction::OpenMap:       ShowMenu(RE::MapMenu::MENU_NAME); logger::info("[global] open map"); break;
            case GlobalAction::OpenInventory: ShowMenu(RE::InventoryMenu::MENU_NAME); logger::info("[global] open inventory"); break;
            case GlobalAction::OpenMagic:     ShowMenu(RE::MagicMenu::MENU_NAME); logger::info("[global] open magic"); break;
            case GlobalAction::OpenSkills:    ShowMenu(RE::StatsMenu::MENU_NAME); logger::info("[global] open skills"); break;
            case GlobalAction::OpenJournal:   ShowMenu(RE::JournalMenu::MENU_NAME); logger::info("[global] open journal"); break;
            case GlobalAction::CloseMap:       HideMenu(RE::MapMenu::MENU_NAME); logger::info("[global] close map"); break;
            case GlobalAction::CloseInventory: HideMenu(RE::InventoryMenu::MENU_NAME); logger::info("[global] close inventory"); break;
            case GlobalAction::CloseMagic:     HideMenu(RE::MagicMenu::MENU_NAME); logger::info("[global] close magic"); break;
            case GlobalAction::CloseSkills:    HideMenu(RE::StatsMenu::MENU_NAME); logger::info("[global] close skills"); break;
            case GlobalAction::CloseJournal:   HideMenu(RE::JournalMenu::MENU_NAME); logger::info("[global] close journal"); break;
            case GlobalAction::QuickSave:
                if (auto* mgr = RE::BGSSaveLoadManager::GetSingleton()) { mgr->Save("Quicksave_Voice"); logger::info("[global] quicksave"); }
                break;
            case GlobalAction::QuickLoad:
                if (auto* mgr = RE::BGSSaveLoadManager::GetSingleton()) { mgr->Load("Quicksave_Voice"); logger::info("[global] quickload"); }
                break;
            default: break;
            }
        });
    }

    void RunWaitHours(int a_hours)
    {
        if (a_hours <= 0) return;
        DoOnMainThread([a_hours](RE::PlayerCharacter* p) {
            // Never wait in combat — vanilla forbids it, and skipping time mid-combat
            // would be exploitable / break encounters.
            if (p && p->IsInCombat()) {
                logger::info("[global] wait {}h refused — in combat", a_hours);
                return;
            }
            auto* cal = RE::Calendar::GetSingleton();
            if (!cal || !cal->gameHour || !cal->gameDaysPassed) {
                logger::warn("[global] wait {}h — Calendar unavailable", a_hours);
                return;
            }
            const float addH     = static_cast<float>(a_hours);
            const float newDays  = cal->gameDaysPassed->value + addH / 24.0f;
            cal->gameDaysPassed->value = newDays;
            // CRITICAL: also sync the engine-PRIVATE day counters. The engine recomputes
            // gameDaysPassed from rawDaysPassed/midnightsPassed on its next Calendar tick;
            // if we leave these stale it can REVERT our jump (the SSE-Engine-Fixes
            // "calendar skipping" bug). rawDaysPassed/midnightsPassed = whole days elapsed.
            const float wholeDays = std::floor(newDays);
            cal->rawDaysPassed   = wholeDays;
            cal->midnightsPassed = static_cast<std::uint32_t>(wholeDays);
            // Hour-of-day with date rollover.
            float h = cal->gameHour->value + addH;
            int dayAdd = static_cast<int>(std::floor(h / 24.0f));
            h -= dayAdd * 24.0f;
            cal->gameHour->value = h;
            if (dayAdd > 0 && cal->gameDay && cal->gameMonth && cal->gameYear) {
                int d = static_cast<int>(cal->gameDay->value) + dayAdd;
                int m = static_cast<int>(cal->gameMonth->value) % 12;
                int y = static_cast<int>(cal->gameYear->value);
                while (d > RE::Calendar::DAYS_IN_MONTH[m]) {
                    d -= RE::Calendar::DAYS_IN_MONTH[m];
                    if (++m >= 12) { m = 0; ++y; }
                }
                cal->gameDay->value   = static_cast<float>(d);
                cal->gameMonth->value = static_cast<float>(m);
                cal->gameYear->value  = static_cast<float>(y);
            }
            // Match vanilla wait: fully restore health/magicka/stamina.
            if (p) {
                if (auto* avo = p->AsActorValueOwner()) {
                    for (auto av : { RE::ActorValue::kHealth, RE::ActorValue::kMagicka, RE::ActorValue::kStamina }) {
                        float cur = avo->GetActorValue(av);
                        float max = avo->GetPermanentActorValue(av);  // base+fortify (no damage)
                        if (max > cur) {
                            avo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, av, max - cur);
                        }
                    }
                }
            }
            // Notify wait-aware scripts/mods (same event vanilla wait fires; not sleep,
            // so no Well Rested). interrupted=false = completed normally.
            if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
                RE::TESWaitStopEvent evt{ false };
                holder->SendEvent(&evt);
            }
            logger::info("[global] waited {} hours (time-skip; H/M/S restored)", a_hours);
        });
    }
}
