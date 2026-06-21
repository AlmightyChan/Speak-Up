#pragma once

// ============================================================================
// CommandGrammar — builds the spoken grammar (phrase list) and the phrase->action
// map from the live roster. Engine-free and unit-testable. Verbs give DBU parity:
//
//   Spells:  "<spell>"                 -> equip right (or cast, if DefaultActionCast)
//            "equip [left|right|dual] <spell>"
//            "left|right|dual <spell>"
//            "cast [left|right] <spell>"
//            "cast dual <spell>" / "dual cast <spell>"   -> dual cast (overcharged)
//            "conjure|summon <spell>"  -> cast (flavor synonyms)
//   Powers:  "<power>" -> equip voice slot;  "use|cast <power>" -> cast
//   Shouts:  "<shout>" -> equip;             "shout|cast <shout>" -> trigger
//
// Collisions (two entries yielding the same phrase) resolve by alias priority
// (full name beats short form), then roster order; the loser is dropped.
// ============================================================================

#include <string>
#include <unordered_map>
#include <vector>

#include "Types.h"

namespace VSC
{
    struct CommandSpec
    {
        std::string name;
        Category    category;
    };

    struct CommandTarget
    {
        std::size_t specIndex;        // index into the input specs / roster
        Action      action;
        Hand        hand;
        bool        dual;
        int         shoutLevel = -1;  // for per-word shout casts: 0/1/2 = word level; -1 = n/a
        bool        stanceOrigin = false;  // bare cast: origin from head (hands down) / right hand (up)
    };

    struct GrammarResult
    {
        std::vector<std::string>                       phrases;  // spoken phrases / fuzzy candidates
        std::unordered_map<std::string, CommandTarget> map;      // phrase -> action
        std::size_t                                    collisions = 0;  // dropped duplicates
        std::vector<std::string>                       conflicts;       // phrases two entries fought over
    };

    // defaultCast: if true, a bare spell/power/shout name CASTS instead of equips.
    // equipHand: hand used by the bare "equip <spell>" verb (and a bare name when
    //   defaultCast is false). Explicit "left/right/dual" always override it.
    GrammarResult BuildGrammar(const std::vector<CommandSpec>& specs, bool defaultCast,
                               Hand equipHand = Hand::Left);
}
