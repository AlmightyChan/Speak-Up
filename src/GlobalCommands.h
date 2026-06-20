#pragma once

// ============================================================================
// GlobalCommands — fixed voice commands that aren't tied to a spell: the listen
// toggle, utility (clear hands/voice), and keybind-FREE game actions (wait,
// quicksave, open menus). "Keybind-free" = we call the engine action directly,
// so it works regardless of the modlist's key bindings.
// ============================================================================

#include "PCH.h"
#include "Types.h"
#include <unordered_map>

namespace VSC
{
    enum class GlobalAction
    {
        None,
        StartListening,   // handled in VoiceController (plugin state)
        StopListening,
        ClearHands,       // unequip spells from both hands
        ClearVoice,       // unequip the voice slot (power/shout)
        Wait,             // open the wait menu
        CloseMenu,        // fallback: close whatever menu is open
        QuickSave,
        QuickLoad,
        OpenMap,
        OpenInventory,
        OpenMagic,
        OpenSkills,
        OpenJournal,
        // Per-menu close ("close/hide <menu>") — closes that specific menu by name.
        CloseMap,
        CloseInventory,
        CloseMagic,
        CloseSkills,
        CloseJournal,
    };

    // Normalized fixed phrases -> action. (Listen-toggle phrases included; the
    // controller special-cases them so they work even while paused.)
    const std::unordered_map<std::string, GlobalAction>& GlobalCommandPhrases();

    // Execute a non-listen global action. Marshals engine work to the main thread.
    void RunGlobalAction(GlobalAction a_action);

    // Advance game time by N hours ("wait N hours"). Marshals to the main thread.
    // This is an instant time-skip via the Calendar globals (clock + date + days-
    // passed advance, so day/night and time-gated things progress); it intentionally
    // does NOT run the rest/regen simulation that the manual Wait menu does.
    void RunWaitHours(int a_hours);
}
