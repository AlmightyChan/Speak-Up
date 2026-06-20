#pragma once

// ============================================================================
// PhraseNormalize — the single normalization applied to BOTH the grammar words
// pushed to Vosk and the phrase->form map keys: lower-case (ASCII), trim,
// collapse internal whitespace to single spaces. Normalizing both sides keeps
// the map keys byte-identical to the text Vosk returns.
// ============================================================================

#include <cctype>
#include <string>
#include <string_view>

namespace VSC
{
    inline std::string NormalizePhrase(std::string_view in)
    {
        std::string out;
        out.reserve(in.size());
        bool prevSpace = false;
        for (char c : in) {
            const unsigned char uc = static_cast<unsigned char>(c);
            if (std::isspace(uc)) {
                if (!prevSpace && !out.empty()) {
                    out.push_back(' ');
                }
                prevSpace = true;
            } else {
                out.push_back(static_cast<char>(std::tolower(uc)));
                prevSpace = false;
            }
        }
        if (!out.empty() && out.back() == ' ') {
            out.pop_back();
        }
        return out;
    }
}
