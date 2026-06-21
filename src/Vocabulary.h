#pragma once

// ============================================================================
// Vocabulary — turns a spell/power/shout DISPLAY NAME into its spoken phrase.
// Engine-free and unit-testable.
//
// DESIGN (per user): the spoken phrase IS the displayed name — no shortening, no
// aliases. "Bound Sword" is spoken "bound sword", not "sword". This keeps the
// grammar small and unambiguous on large modded spell lists (aliases would cause
// cross-spell collisions). We only normalize what isn't spoken: lower-case,
// collapse whitespace, drop punctuation, and remove apostrophes ("vampire's" ->
// "vampires") since the apostrophe is silent.
// ============================================================================

#include <string>
#include <vector>

namespace VSC
{
    struct Alias
    {
        std::string text;      // normalized spoken form
        int         priority;  // 0 = primary (exact sanitized name); 1 = OOV variant
    };

    // Lower-case, drop apostrophes ("vampire's" -> "vampires"), turn other
    // punctuation/brackets/digits into separators, collapse whitespace. e.g.
    // "<Unbind Slot>" -> "unbind slot", "Vampire's Sight" -> "vampires sight".
    std::string SanitizeName(const std::string& displayName);

    // Returns the primary spoken phrase (priority 0) plus up to two OOV variant
    // phrases (priority 1) for tokens unlikely to be heard cleanly by the speech
    // model. Variants are produced by:
    //   (a) compound-word splitting (e.g. "featherfall" -> "feather fall")
    //   (b) light grapheme respelling (e.g. "phantasm" -> "fantasm")
    // Returns an empty vector if the name sanitizes to nothing.
    std::vector<Alias> GenerateAliases(const std::string& displayName);
}
