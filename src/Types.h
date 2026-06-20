#pragma once

// ============================================================================
// Pure value types shared between the engine-facing code and the (engine-free,
// unit-testable) vocabulary/grammar generation. NO RE::/SKSE:: dependencies here
// so CommandGrammar/Vocabulary can be tested out-of-game.
// ============================================================================

#include <string>

namespace VSC
{
    enum class Category
    {
        Spell,
        Power,
        Shout
    };

    inline const char* CategoryName(Category c)
    {
        switch (c) {
        case Category::Spell: return "Spell";
        case Category::Power: return "Power";
        case Category::Shout: return "Shout";
        default:              return "?";
        }
    }

    // Which hand(s) an equip/cast targets. Ignored for shouts and two-handed spells.
    enum class Hand
    {
        Right,
        Left,
        Both
    };

    inline const char* HandName(Hand h)
    {
        switch (h) {
        case Hand::Right: return "R";
        case Hand::Left:  return "L";
        case Hand::Both:  return "LR";
        default:          return "?";
        }
    }

    // What a recognized phrase does.
    enum class Action
    {
        Equip,   // place in the chosen hand/voice slot, do not fire
        Cast     // fire immediately (auto-cast); dual flag = overcharged dual cast
    };

    inline const char* ActionName(Action a)
    {
        return a == Action::Cast ? "Cast" : "Equip";
    }
}
