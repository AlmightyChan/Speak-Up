#include "Vocabulary.h"
#include "PhraseNormalize.h"

#include <cctype>
#include <cstring>
#include <sstream>

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

    namespace
    {
        // -----------------------------------------------------------------------
        // OovRespell — light English grapheme-to-phoneme approximation for tokens
        // that look unlikely to be heard cleanly by the speech model (fantasy-game
        // names, compound words that got merged, Latin-rooted spell names).
        //
        // Strategy: apply a small set of reliable English digraph/vowel rules so
        // the recognizer's acoustic model is more likely to match the spoken sound. We only
        // emit the respelling as a variant (priority 1), never replacing the raw
        // name (priority 0). Returns empty string if no transformation was made.
        // -----------------------------------------------------------------------
        std::string OovRespell(const std::string& w)
        {
            // w is already lower-case (comes from SanitizeName / NormalizePhrase).
            std::string s = w;
            auto repl = [&](const char* from, const char* to) {
                std::string f = from, t = to;
                std::size_t p = 0;
                while ((p = s.find(f, p)) != std::string::npos) {
                    s.replace(p, f.size(), t);
                    p += t.size();
                }
            };

            // Common non-English digraphs / silent letters found in TES spell names:
            repl("th",  "th");   // keep (already natural English)
            repl("ck",  "k");    // "shock" stays "shock" but "tronach"->no change; "warlockk"->noop
            repl("ph",  "f");    // "morph" -> "morf", "phantasm" -> "fantasm"
            repl("gh",  "");     // "blight" -> "blit", "wraith" -> "wraith" (no gh)
            repl("kn",  "n");    // "knight" -> "night"
            repl("wr",  "r");    // "wraith" -> "raith"
            repl("ae",  "ee");   // "daedra" -> "deedra" (actually helped in practice)
            repl("oo",  "oo");   // keep
            repl("ou",  "ow");   // "conjour" -> "conjow" -- but conjure has 'ure', skip
            repl("ue",  "oo");   // "conjure" -> "conjoor" -- deliberate
            repl("tion","shun"); // "levitation" -> "levitashun"
            repl("thr", "thr");  // keep
            repl("str", "str");  // keep
            repl("sch", "sk");   // rare but "school" like names
            repl("ch",  "k");    // "chaurus" -> "kaurus", "lich" -> "lik"

            // Final -e is often silent; keep as-is (removing breaks more than helps)

            return (s == w) ? std::string{} : s;
        }

        // -----------------------------------------------------------------------
        // SplitCompound — if the sanitized token looks like a merged compound word,
        // return a space-separated split; else empty.
        //
        // Detection heuristic: look for lower->upper case transitions in the
        // ORIGINAL display name token (e.g. "FeatherFall"), OR for a known-split
        // interior boundary in the all-lower sanitized form (dictionary of common
        // TES spell-word prefixes/suffixes that form compound names). We only split
        // single tokens (no space in `token`). Cap at one split point to keep the
        // variant count stable.
        // -----------------------------------------------------------------------
        std::string SplitCompound(const std::string& rawToken,     // original casing
                                  const std::string& lowerToken)   // already lower-case
        {
            if (lowerToken.empty() || lowerToken.find(' ') != std::string::npos) return {};

            // --- Pass 1: camelCase / PascalCase boundary in original casing --------
            // e.g. "FeatherFall" -> "feather fall", "FireBolt" -> "fire bolt"
            {
                std::string split;
                bool didSplit = false;
                for (std::size_t i = 0; i < rawToken.size(); ++i) {
                    unsigned char c  = static_cast<unsigned char>(rawToken[i]);
                    unsigned char cn = (i + 1 < rawToken.size())
                        ? static_cast<unsigned char>(rawToken[i + 1]) : 0;
                    split.push_back(static_cast<char>(std::tolower(c)));
                    // Insert space at lower->UPPER transition (e.g. r|F in "FeatherFall")
                    // but not at the very first char, and not between two uppers (acronym).
                    if (i > 0 && cn != 0 && std::islower(c) && std::isupper(cn)) {
                        split.push_back(' ');
                        didSplit = true;
                    }
                }
                if (didSplit) return NormalizePhrase(split);
            }

            // --- Pass 2: known spell-word boundaries in the lower-case token -------
            // Common TES compound-name components that get merged by mod authors
            // (e.g. LoreRim: "Featherfall", "Thunderbolt", "Frostfall", etc.).
            // We try each prefix; if it matches the START of the token AND a suffix
            // of reasonable length (>= 3 chars) remains, emit the split.
            static const char* kPrefixes[] = {
                "feather","thunder","frost","fire","ice","flame","storm","chain",
                "shock","force","bound","soul","drain","absorb","banish","calm",
                "frenzy","rally","detect","muffle","cloak","ward","rune","light",
                "dark","dead","undead","tele","tele","vampire","were","blood",
                "death","life","mind","spirit","shadow","ghost","wind","earth",
                "water","poison","curse","paralyze","silence","reflect","shield",
                "power","summon","raise","reanimate","command","repel","expel",
                "turn","control","open","lock","reveal","conceal","mark","recall",
                "levitate","swift","slow","speed","heavy","stone","iron","steel",
                nullptr
            };
            const std::size_t n = lowerToken.size();
            for (const char** pfx = kPrefixes; *pfx; ++pfx) {
                std::size_t plen = std::strlen(*pfx);
                if (n > plen + 3 && lowerToken.compare(0, plen, *pfx) == 0) {
                    // Make sure we don't split "firebolt" -> "fire bolt" if the suffix
                    // is already a real word (just emit the split; the recognizer handles it).
                    std::string candidate = std::string(*pfx) + ' ' + lowerToken.substr(plen);
                    return NormalizePhrase(candidate);
                }
            }

            return {};
        }
    }  // anonymous namespace

    // ---------------------------------------------------------------------------
    // GenerateAliases — returns the raw sanitized name (priority 0) plus up to
    // two OOV variant phrases (priority 1) so out-of-vocabulary spell names have
    // a better chance of being recognized without replacing exact matches.
    //
    // Rules:
    //   1. The raw sanitized display name is ALWAYS alias[0] (priority 0).
    //   2. For each whitespace-token in the sanitized name, attempt:
    //      (a) compound split (e.g. "featherfall" -> "feather fall")
    //      (b) OOV grapheme respelling (e.g. "atronach" -> "atronak")
    //      Whichever produces a phrase different from base is added (capped at 2
    //      total variants so the grammar doesn't explode).
    // ---------------------------------------------------------------------------
    std::vector<Alias> GenerateAliases(const std::string& displayName)
    {
        std::vector<Alias> out;
        std::string base = SanitizeName(displayName);
        if (base.empty()) return out;

        out.push_back({ base, 0 });  // primary: exact sanitized name

        // Tokenise the raw display name (respecting original casing for SplitCompound).
        // We need both the original-cased tokens AND the lower-case base tokens.
        struct Token { std::string raw; std::string lower; };
        std::vector<Token> tokens;
        {
            // Re-walk the display name to gather original-cased alpha-only tokens.
            std::string cur;
            for (char c : displayName) {
                unsigned char uc = static_cast<unsigned char>(c);
                if (c == '\'') continue;  // match SanitizeName's apostrophe rule
                if (std::isalpha(uc)) {
                    cur.push_back(c);
                } else {
                    if (!cur.empty()) { tokens.push_back({ cur, NormalizePhrase(cur) }); cur.clear(); }
                }
            }
            if (!cur.empty()) tokens.push_back({ cur, NormalizePhrase(cur) });
        }

        // Build variant phrases by replacing one token at a time.
        // We keep a "base tokens" (all lower-case) list to substitute into.
        std::vector<std::string> baseToks;
        for (const auto& t : tokens) baseToks.push_back(t.lower);

        int variantsAdded = 0;
        for (std::size_t i = 0; i < tokens.size() && variantsAdded < 2; ++i) {
            // Try compound split first (higher priority variant)
            std::string split = SplitCompound(tokens[i].raw, tokens[i].lower);
            if (!split.empty() && split != tokens[i].lower) {
                // Reconstruct the full phrase with this token replaced
                std::string variant;
                for (std::size_t j = 0; j < baseToks.size(); ++j) {
                    if (!variant.empty()) variant += ' ';
                    variant += (j == i) ? split : baseToks[j];
                }
                variant = NormalizePhrase(variant);
                if (variant != base) {
                    out.push_back({ variant, 1 });
                    ++variantsAdded;
                    continue;  // split already covers this token; skip OOV respell for it
                }
            }

            // Try OOV grapheme respelling
            if (variantsAdded < 2) {
                std::string resp = OovRespell(tokens[i].lower);
                if (!resp.empty() && resp != tokens[i].lower) {
                    std::string variant;
                    for (std::size_t j = 0; j < baseToks.size(); ++j) {
                        if (!variant.empty()) variant += ' ';
                        variant += (j == i) ? resp : baseToks[j];
                    }
                    variant = NormalizePhrase(variant);
                    if (variant != base) {
                        out.push_back({ variant, 1 });
                        ++variantsAdded;
                    }
                }
            }
        }

        return out;
    }
}
