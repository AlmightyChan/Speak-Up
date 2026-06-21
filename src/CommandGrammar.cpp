#include "CommandGrammar.h"
#include "Vocabulary.h"
#include "PhraseNormalize.h"

namespace VSC
{
    namespace
    {
        // A verb template: a spoken prefix ("" = bare name) and the action it maps to.
        struct Verb
        {
            const char* prefix;       // before the name ("" for none)
            Action      action;
            Hand        hand;
            bool        dual;
            const char* suffix = "";  // after the name ("" for none) — e.g. equip <name> right
            bool        stanceOrigin = false;  // bare cast: head when hands down, right hand when up
        };

        std::vector<Verb> VerbsFor(Category cat, bool defaultCast, Hand equipHand)
        {
            // "Unmarked" verbs (bare name, and bare hand prefixes left/right/dual)
            // follow the default. Explicit "equip"/"cast" prefixes always override.
            const Action bare = defaultCast ? Action::Cast : Action::Equip;
            // Bare "dual <spell>" => dual-CAST in cast-default; equip-both in equip-default.
            const bool   bareDual = defaultCast;
            // A bare name's hand: cast-default casts from the right; equip-default uses
            // the configured equip hand.
            const Hand   bareHand = defaultCast ? Hand::Right : equipHand;
            switch (cat) {
            case Category::Spell:
                return {
                    // Bare cast (no hand spoken): origin follows weapon stance —
                    // head when hands are down, right hand when drawn. Explicit
                    // left/right/dual below always use that exact hand.
                    { "",               bare,          bareHand,    false, "", defaultCast },
                    { "right",          bare,          Hand::Right, false    },
                    { "left",           bare,          Hand::Left,  false    },
                    { "dual",           bare,          Hand::Both,  bareDual },
                    // Equip uses a SUFFIX hand: "equip <name>", "equip <name> right/left/dual".
                    { "equip",          Action::Equip, equipHand,   false, ""      },
                    { "equip",          Action::Equip, Hand::Right, false, "right" },
                    { "equip",          Action::Equip, Hand::Left,  false, "left"  },
                    { "equip",          Action::Equip, Hand::Both,  false, "dual"  },
                    { "cast",           Action::Cast,  Hand::Right, false    },
                    { "cast right",     Action::Cast,  Hand::Right, false    },
                    { "cast left",      Action::Cast,  Hand::Left,  false    },
                    { "cast dual",      Action::Cast,  Hand::Both,  true     },
                    { "dual cast",      Action::Cast,  Hand::Both,  true     },
                    { "conjure",        Action::Cast,  Hand::Right, false    },
                    { "summon",         Action::Cast,  Hand::Right, false    },
                };
            case Category::Power:
                return {
                    { "",      bare,          Hand::Right, false },
                    { "equip", Action::Equip, Hand::Right, false },
                    { "use",   Action::Cast,  Hand::Right, false },
                    { "cast",  Action::Cast,  Hand::Right, false },
                };
            case Category::Shout:
                // Saying the shout's NAME (e.g. "Unrelenting Force") casts it at the
                // HIGHEST word level you currently know (shoutLevel -1 = highest unlocked,
                // resolved in CastShoutNow) — a reliable fallback that sidesteps Dovahzul
                // recognition. "equip <name>" equips it; "cast/shout <name>" force a cast.
                // The per-word Dovahzul/English phrases (fus / fus ro / fus ro dah) are
                // still added engine-side in VoiceController for tier-specific casts.
                return {
                    { "",      bare,          Hand::Right, false },  // bare name -> cast highest (default) or equip
                    { "cast",  Action::Cast,  Hand::Right, false },
                    { "shout", Action::Cast,  Hand::Right, false },
                    { "equip", Action::Equip, Hand::Right, false },
                };
            }
            return {};
        }

        std::string Compose(const char* prefix, const std::string& alias, const char* suffix)
        {
            std::string s;
            if (prefix && *prefix) s += prefix, s += ' ';
            s += alias;
            if (suffix && *suffix) s += ' ', s += suffix;
            return NormalizePhrase(s);
        }
    }

    GrammarResult BuildGrammar(const std::vector<CommandSpec>& specs, bool defaultCast, Hand equipHand)
    {
        GrammarResult result;
        // phrase -> winning alias priority (lower wins; ties keep earlier spec).
        std::unordered_map<std::string, int> bestPriority;

        for (std::size_t i = 0; i < specs.size(); ++i) {
            const auto& spec = specs[i];
            auto aliases = GenerateAliases(spec.name);
            auto verbs = VerbsFor(spec.category, defaultCast, equipHand);

            for (const auto& alias : aliases) {
                for (const auto& verb : verbs) {
                    std::string phrase = Compose(verb.prefix, alias.text, verb.suffix);
                    if (phrase.empty()) continue;

                    auto it = bestPriority.find(phrase);
                    if (it != bestPriority.end()) {
                        ++result.collisions;
                        if (result.conflicts.size() < 50) result.conflicts.push_back(phrase);
                        if (alias.priority >= it->second) {
                            continue;  // existing winner keeps the phrase (deterministic)
                        }
                        // else: better claim overrides (lower priority value wins)
                    }
                    bestPriority[phrase] = alias.priority;
                    result.map[phrase] = CommandTarget{ i, verb.action, verb.hand, verb.dual, -1, verb.stanceOrigin };
                }
            }
        }

        result.phrases.reserve(result.map.size());
        for (const auto& kv : result.map) {
            result.phrases.push_back(kv.first);
        }
        return result;
    }
}
