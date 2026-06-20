#include "PCH.h"
#include "TestHarness.h"

#if VSC_ENABLE_TEST_HARNESS

#include "SpellRoster.h"
#include "Equipper.h"
#include "CommandGrammar.h"
#include "VoiceController.h"
#include <Windows.h>

namespace logger = SKSE::log;

namespace VSC
{
    namespace
    {
        // ------------------------------------------------------------------
        // Dev hotkeys are INI-CONFIGURABLE (with an OPTIONAL modifier gate).
        // Defaults: top-row 2 = dump, top-row 3 = equip-next, no modifier.
        // Remap via Data/SKSE/Plugins/VoiceSpellcasting.ini:
        //
        //   [Debug]
        //   ModifierDX=0      ; 0 = no modifier (set e.g. 0x38 = Left Alt to gate)
        //   DumpKeyDX=0x03    ; top-row 2
        //   EquipKeyDX=0x04   ; top-row 3
        //
        // DX scan codes, NOT VK codes.
        // ------------------------------------------------------------------
        std::uint32_t g_modifierDX = 0x00;  // no modifier
        std::uint32_t g_dumpDX     = 0x03;  // top-row 2
        std::uint32_t g_equipDX    = 0x04;  // top-row 3
        bool          g_modifierDown = false;

        std::vector<RosterEntry> g_roster;
        std::size_t              g_equipCursor = 0;

        void LoadConfig()
        {
            // Resolve against the game root so MO2's VFS maps it correctly.
            const char* path = "Data\\SKSE\\Plugins\\VoiceSpellcasting.ini";
            auto readKey = [&](const char* key, std::uint32_t fallback) -> std::uint32_t {
                // -1 sentinel distinguishes "absent" from a real 0 value.
                UINT v = ::GetPrivateProfileIntA("Debug", key, 0xFFFFFFFF, path);
                return v == 0xFFFFFFFF ? fallback : static_cast<std::uint32_t>(v);
            };
            g_modifierDX = readKey("ModifierDX", g_modifierDX);
            g_dumpDX     = readKey("DumpKeyDX", g_dumpDX);
            g_equipDX    = readKey("EquipKeyDX", g_equipDX);
        }

        void DumpRoster()
        {
            g_roster = BuildRoster();
            std::size_t spells = 0, powers = 0, shouts = 0;
            for (const auto& e : g_roster) {
                switch (e.category) {
                case Category::Spell: ++spells; break;
                case Category::Power: ++powers; break;
                case Category::Shout: ++shouts; break;
                }
            }
            logger::info("[dump] roster: {} total ({} spells, {} powers, {} shouts)",
                g_roster.size(), spells, powers, shouts);
            for (std::size_t i = 0; i < g_roster.size(); ++i) {
                const auto& e = g_roster[i];
                logger::info("[dump]   [{:>3}] {:<6} 0x{:08X}  {}",
                    i, CategoryName(e.category), e.formID, e.name);
            }

            // Dump the LIVE grammar actually in use (includes per-word shout phrases +
            // global commands, which BuildGrammar alone does not).
            VoiceController::Get().DumpGrammar();
        }

        void EquipNext()
        {
            if (g_roster.empty()) {
                g_roster = BuildRoster();
            }
            if (g_roster.empty()) {
                logger::info("[equip] roster empty — nothing to equip");
                return;
            }
            const auto& entry = g_roster[g_equipCursor % g_roster.size()];
            logger::info("[equip] cursor {} -> {} '{}'",
                g_equipCursor % g_roster.size(), CategoryName(entry.category), entry.name);
            EquipEntry(entry, Hand::Right);
            ++g_equipCursor;
        }

        class InputHandler : public RE::BSTEventSink<RE::InputEvent*>
        {
        public:
            static InputHandler* GetSingleton()
            {
                static InputHandler singleton;
                return &singleton;
            }

            RE::BSEventNotifyControl ProcessEvent(
                RE::InputEvent* const*               a_event,
                RE::BSTEventSource<RE::InputEvent*>*) override
            {
                if (!a_event) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                for (auto* e = *a_event; e; e = e->next) {
                    auto* button = e->AsButtonEvent();
                    if (!button || e->GetDevice() != RE::INPUT_DEVICE::kKeyboard) {
                        continue;
                    }
                    const std::uint32_t dx = button->GetIDCode();

                    // Track the modifier's held state (down + up).
                    if (g_modifierDX != 0 && dx == g_modifierDX) {
                        g_modifierDown = button->IsPressed();
                        continue;
                    }
                    if (!button->IsDown()) {
                        continue;  // act on the initial press only
                    }
                    // Gate: if a modifier is configured, it must be held.
                    if (g_modifierDX != 0 && !g_modifierDown) {
                        continue;
                    }
                    if (dx == g_dumpDX) {
                        DumpRoster();
                    } else if (dx == g_equipDX) {
                        EquipNext();
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }

        private:
            InputHandler() = default;
        };
    }

    void InstallTestHarness()
    {
        LoadConfig();
        if (auto* idm = RE::BSInputDeviceManager::GetSingleton()) {
            idm->AddEventSink(InputHandler::GetSingleton());
            if (g_modifierDX != 0) {
                logger::info("[harness] hotkeys installed (modifier-gated) — "
                             "hold modifier 0x{:X} + dump 0x{:X} / equip-next 0x{:X}",
                    g_modifierDX, g_dumpDX, g_equipDX);
            } else {
                logger::info("[harness] hotkeys installed (no modifier) — "
                             "dump 0x{:X} / equip-next 0x{:X}", g_dumpDX, g_equipDX);
            }
        } else {
            logger::error("[harness] BSInputDeviceManager unavailable — hotkeys NOT installed");
        }
    }
}

#endif // VSC_ENABLE_TEST_HARNESS
