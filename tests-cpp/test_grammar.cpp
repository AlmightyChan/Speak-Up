// Out-of-game unit tests for Vocabulary + CommandGrammar (pure, no engine).
#include "Vocabulary.h"
#include "CommandGrammar.h"

#include <cstdio>
#include <algorithm>

using namespace VSC;

static int g_fail = 0;
static int g_total = 0;

#define CHECK(cond, msg)                                              \
    do {                                                              \
        ++g_total;                                                    \
        if (!(cond)) { ++g_fail; std::printf("FAIL: %s\n", msg); }    \
    } while (0)

static bool HasAlias(const std::vector<Alias>& v, const char* t)
{
    return std::any_of(v.begin(), v.end(), [&](const Alias& a) { return a.text == t; });
}

static bool HasPhrase(const GrammarResult& g, const char* p)
{
    return g.map.find(p) != g.map.end();
}

static const CommandTarget* Target(const GrammarResult& g, const char* p)
{
    auto it = g.map.find(p);
    return it == g.map.end() ? nullptr : &it->second;
}

int main()
{
    // ---- SanitizeName -----------------------------------------------------
    CHECK(SanitizeName("<Unbind Slot>") == "unbind slot", "sanitize strips brackets");
    CHECK(SanitizeName("Fire  Ball") == "fire ball", "sanitize collapses whitespace");
    CHECK(SanitizeName("Vampire's Sight") == "vampires sight", "sanitize drops apostrophe");

    // ---- GenerateAliases: the displayed name IS the phrase (no shortening) -
    auto a = GenerateAliases("Vampire's Sight");
    CHECK(a.size() == 1, "no aliases: exactly one phrase");
    CHECK(HasAlias(a, "vampires sight"), "phrase = sanitized full name");
    CHECK(!HasAlias(a, "sight"), "no tail short form");

    auto b = GenerateAliases("Bound Sword");
    CHECK(b.size() == 1 && HasAlias(b, "bound sword"), "bound sword stays 'bound sword'");
    CHECK(!HasAlias(b, "sword"), "bound sword NOT shortened to 'sword'");

    // ---- BuildGrammar (default = equip) -----------------------------------
    std::vector<CommandSpec> specs = {
        { "Firebolt", Category::Spell },
        { "Unrelenting Force", Category::Shout },
        { "Vampire's Sight", Category::Power },
    };
    GrammarResult g = BuildGrammar(specs, /*defaultCast*/ false);

    CHECK(HasPhrase(g, "firebolt"), "phrase: bare spell name");
    CHECK(HasPhrase(g, "cast firebolt"), "phrase: cast verb");
    CHECK(HasPhrase(g, "equip left firebolt"), "phrase: equip left");
    CHECK(HasPhrase(g, "left firebolt"), "phrase: left short");
    CHECK(HasPhrase(g, "dual cast firebolt"), "phrase: dual cast");
    CHECK(HasPhrase(g, "conjure firebolt"), "phrase: conjure synonym");

    if (auto* t = Target(g, "firebolt")) {
        CHECK(t->action == Action::Equip, "bare = equip by default");
        CHECK(t->hand == Hand::Right, "bare = right hand");
    } else CHECK(false, "bare firebolt target exists");

    if (auto* t = Target(g, "cast left firebolt")) {
        CHECK(t->action == Action::Cast, "cast left = cast");
        CHECK(t->hand == Hand::Left, "cast left = left");
    } else CHECK(false, "cast left target exists");

    if (auto* t = Target(g, "dual cast firebolt")) {
        CHECK(t->action == Action::Cast && t->dual, "dual cast = cast + dual flag");
    } else CHECK(false, "dual cast target exists");

    // Equip-default: bare "dual <spell>" equips both hands (NOT dual cast).
    if (auto* t = Target(g, "dual firebolt")) {
        CHECK(t->action == Action::Equip && t->hand == Hand::Both && !t->dual,
              "equip-default: 'dual X' = equip both hands");
    } else CHECK(false, "dual firebolt target exists");
    if (auto* t = Target(g, "equip dual firebolt")) {
        CHECK(t->action == Action::Equip && t->hand == Hand::Both, "equip dual = equip both");
    } else CHECK(false, "equip dual target exists");

    // Shout verbs
    CHECK(HasPhrase(g, "shout unrelenting force"), "phrase: shout verb");
    if (auto* t = Target(g, "shout unrelenting force"))
        CHECK(t->action == Action::Cast, "shout verb = cast/trigger");

    // Power verbs
    CHECK(HasPhrase(g, "use vampires sight"), "phrase: power use verb");

    // ---- BuildGrammar (default = cast) ------------------------------------
    GrammarResult gc = BuildGrammar(specs, /*defaultCast*/ true);
    if (auto* t = Target(gc, "firebolt"))
        CHECK(t->action == Action::Cast, "cast-default: bare name casts");
    // Cast-default: "dual <spell>" = dual CAST.
    if (auto* t = Target(gc, "dual firebolt"))
        CHECK(t->action == Action::Cast && t->dual, "cast-default: 'dual X' = dual cast");
    else CHECK(false, "cast-default dual firebolt exists");
    // "equip" prefix still equips even in cast-default.
    if (auto* t = Target(gc, "equip firebolt"))
        CHECK(t->action == Action::Equip, "cast-default: 'equip X' still equips");
    if (auto* t = Target(gc, "equip dual firebolt"))
        CHECK(t->action == Action::Equip && t->hand == Hand::Both, "cast-default: equip dual = equip both");

    // ---- Distinct names don't interfere; identical names resolve to first --
    std::vector<CommandSpec> dup = {
        { "Soul Sight", Category::Spell },
        { "Vampire's Sight", Category::Spell },
    };
    GrammarResult gd = BuildGrammar(dup, false);
    CHECK(HasPhrase(gd, "soul sight"), "distinct: first name present");
    CHECK(HasPhrase(gd, "vampires sight"), "distinct: second name present");
    CHECK(!HasPhrase(gd, "sight"), "no bare 'sight' phrase (no aliases)");

    // Two mods defining the SAME display name -> phrase resolves to the first spec.
    std::vector<CommandSpec> same = {
        { "Fire Bolt", Category::Spell },
        { "Fire Bolt", Category::Spell },
    };
    GrammarResult gs = BuildGrammar(same, false);
    if (auto* t = Target(gs, "fire bolt"))
        CHECK(t->specIndex == 0, "identical names -> first spec wins");
    else CHECK(false, "identical-name phrase exists");

    std::printf("\n%d/%d checks passed.\n", g_total - g_fail, g_total);
    return g_fail == 0 ? 0 : 1;
}
