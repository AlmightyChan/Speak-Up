#include "Vocabulary.h"
#include "PhraseNormalize.h"

#include <cctype>

namespace VSC
{
    std::string SanitizeName(const std::string& displayName)
    {
        std::string kept;
        kept.reserve(displayName.size());
        for (char c : displayName) {
            unsigned char uc = static_cast<unsigned char>(c);
            if (c == '\'') {
                // Apostrophe is silent: drop it with no separator ("vampire's" -> "vampires").
                continue;
            }
            if (std::isalpha(uc) || std::isspace(uc)) {
                kept.push_back(c);
            } else {
                kept.push_back(' ');  // punctuation/brackets/digits become separators
            }
        }
        return NormalizePhrase(kept);
    }

    std::vector<Alias> GenerateAliases(const std::string& displayName)
    {
        std::vector<Alias> out;
        std::string base = SanitizeName(displayName);
        if (!base.empty()) {
            out.push_back({ base, 0 });  // the displayed name IS the phrase — no aliases
        }
        return out;
    }
}
