#pragma once

// ============================================================================
// FuzzyMatch.h — phonetic + edit-distance fuzzy matcher
//
// Maps a free-text ASR transcript (e.g. from sherpa-onnx) to the closest known
// command phrase from the live grammar.
//
// API:
//   VSC::FuzzyResult VSC::BestFuzzyMatch(transcript, candidates)
//
// Algorithm:
//   score = 0.25*lev_raw + 0.15*phon_raw + 0.25*lev_folded + 0.20*phon_folded
//         + 0.15*anchor_bonus
//
// Phonetic similarity: per-word Double Metaphone (primary code) then Jaccard on
// the code multisets of the two strings.  Handles "natronac"~"atronach",
// "fire bolt"~"firebolt", "spent"~"sprint", etc.
//
// The *_folded channels run the same two similarities on confusable-consonant-
// folded copies of the strings (see FoldConfusables) so the open-vocab ASR's
// labial swaps — "Fus" heard as 'boos'/'pusra', "Wuld" as 'bold' — don't sink an
// otherwise-correct word. Kept as SEPARATE weighted channels (not a replacement)
// so the raw signal still dominates true matches.
//
// Self-contained, dependency-free, header-only.
// ============================================================================

// Guard against Windows min/max macros before pulling in any standard headers.
#ifdef min
#  undef min
#endif
#ifdef max
#  undef max
#endif

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

// Re-suppress the macros after our STL includes (Windows.h may be included
// transitively later; we don't want to permanently remove the macros).
// We use our own small wrappers below instead of std::min/max calls.

namespace VSC
{
    struct FuzzyResult
    {
        std::string phrase;
        double      score;   // [0, 1]; 1 = perfect match
    };

    namespace detail
    {
        // Small min/max wrappers that are immune to Windows macro pollution.
        template<class T> inline T fm_min(T a, T b) { return a < b ? a : b; }
        template<class T> inline T fm_max(T a, T b) { return a > b ? a : b; }
        template<class T> inline T fm_min3(T a, T b, T c) { return fm_min(a, fm_min(b, c)); }

        // -----------------------------------------------------------------------
        // Normalize: lower-case, collapse runs of non-alnum to a single space,
        // trim leading/trailing whitespace.
        // -----------------------------------------------------------------------
        inline std::string Normalize(const std::string& s)
        {
            std::string out;
            out.reserve(s.size());
            bool prevSpace = true;
            for (unsigned char c : s) {
                if (std::isalnum(c)) {
                    out += static_cast<char>(std::tolower(c));
                    prevSpace = false;
                } else {
                    if (!prevSpace) {
                        out += ' ';
                        prevSpace = true;
                    }
                }
            }
            if (!out.empty() && out.back() == ' ') out.pop_back();
            return out;
        }

        // -----------------------------------------------------------------------
        // FoldConfusables — collapse acoustically-confusable consonants to one
        // canonical letter so the matcher tolerates the open-vocab ASR's most
        // common substitutions. The streaming English model frequently mis-hears
        // the LABIAL family across each other on these short, breathy command
        // words (logs: "Fus" -> 'boos'/'pusra', "Wuld" -> 'bold'). Double Metaphone
        // already merges b/p and f/v, but NOT across the {b,p}|{f,v}|{w} boundary,
        // and Levenshtein is prefix-sensitive, so a swapped first consonant craters
        // both channels. Folding {b,p,f,v,w} -> 'b' lets 'boos'~'foos' and
        // 'bold'~'woold' read as near-identical. Conservative on purpose: only the
        // family the data shows the ASR actually confuses ('m' stays distinct — its
        // nasal quality isn't confused with the stops/fricatives).
        // -----------------------------------------------------------------------
        inline std::string FoldConfusables(const std::string& s)
        {
            std::string out;
            out.reserve(s.size());
            for (char c : s) {
                switch (c) {
                    case 'b': case 'p': case 'f': case 'v': case 'w': out += 'b'; break;
                    default:  out += c; break;
                }
            }
            return out;
        }

        // -----------------------------------------------------------------------
        // Despace — drop ALL spaces. The folded channels run on despaced copies so a
        // run-together transcription matches a multi-word phrase: said quickly, the ASR
        // emits "Fus Ro Dah" as one token "pusrodah", which scores poorly word-by-word
        // against the spaced "fus ro dah" — but despace+fold makes both "busrodah".
        // -----------------------------------------------------------------------
        inline std::string Despace(const std::string& s)
        {
            std::string out;
            out.reserve(s.size());
            for (char c : s) if (c != ' ') out += c;
            return out;
        }

        // -----------------------------------------------------------------------
        // Tokenize on spaces
        // -----------------------------------------------------------------------
        inline std::vector<std::string> Tokenize(const std::string& s)
        {
            std::vector<std::string> tokens;
            std::size_t i = 0;
            while (i < s.size()) {
                while (i < s.size() && s[i] == ' ') ++i;
                std::size_t j = i;
                while (j < s.size() && s[j] != ' ') ++j;
                if (j > i) tokens.push_back(s.substr(i, j - i));
                i = j;
            }
            return tokens;
        }

        // -----------------------------------------------------------------------
        // Levenshtein similarity on characters.
        // sim = 1 - edit_distance / max(|a|, |b|)
        // Single-row DP — O(|a| * |b|) time, O(|b|) space.
        // -----------------------------------------------------------------------
        inline double LevenshteinSim(const std::string& a, const std::string& b)
        {
            if (a.empty() && b.empty()) return 1.0;
            const std::size_t na = a.size();
            const std::size_t nb = b.size();
            const std::size_t maxLen = fm_max(na, nb);

            std::vector<std::size_t> row(nb + 1);
            for (std::size_t j = 0; j <= nb; ++j) row[j] = j;

            for (std::size_t i = 1; i <= na; ++i) {
                std::size_t prev = i;
                for (std::size_t j = 1; j <= nb; ++j) {
                    const std::size_t sub = (a[i - 1] == b[j - 1]) ? 0u : 1u;
                    const std::size_t cur = fm_min3(row[j] + 1, prev + 1, row[j - 1] + sub);
                    row[j - 1] = prev;
                    prev = cur;
                }
                row[nb] = prev;
            }
            return 1.0 - static_cast<double>(row[nb]) / static_cast<double>(maxLen);
        }

        // -----------------------------------------------------------------------
        // Double Metaphone — primary code only.
        // Public-domain algorithm by Lawrence Philips, ported to C++.
        // Only the primary code path is implemented; the secondary ("alternate")
        // code is omitted for brevity — the primary code is sufficient here.
        //
        // Input: a single word (letters only; upper-cased internally).
        // Output: primary phonetic code string.
        // -----------------------------------------------------------------------

        inline bool dm_IsVowel(char c)
        {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return c=='A' || c=='E' || c=='I' || c=='O' || c=='U' || c=='Y';
        }

        // s is the working (upper-cased, sentinel-padded) string; pos is 0-based.
        inline bool dm_StringAt(const std::string& s, int pos,
                                 std::initializer_list<const char*> subs)
        {
            if (pos < 0 || pos >= static_cast<int>(s.size())) return false;
            for (const char* sub : subs) {
                const std::size_t len = std::strlen(sub);
                if (static_cast<std::size_t>(pos) + len <= s.size() &&
                    s.compare(static_cast<std::size_t>(pos), len, sub) == 0)
                    return true;
            }
            return false;
        }

        inline std::string DoubleMetaphone(const std::string& src)
        {
            if (src.empty()) return "";

            // Upper-case and prepend two sentinel spaces (classic DM trick so
            // lookbacks like s[current-1] are always safe from index 2 onwards).
            std::string s = "  ";
            for (unsigned char c : src)
                s += static_cast<char>(std::toupper(c));
            // Append trailing sentinel spaces so all lookahead reads are in-bounds.
            // The widest single-character advance is 'W' + WICZ/WITZ -> cur += 4,
            // after which the loop condition reads s[cur]; branches also read s[cur+1]
            // and dm_StringAt reads up to s[cur+3]. Five spaces covers cur+4 worst-case.
            s += "     ";

            const int length = static_cast<int>(s.size());
            std::string primary;
            primary.reserve(8);

            // Real content starts at index 2.
            int cur = 2;

            // Skip initial silent pairs: GN, KN, PN, AE, WR.
            if (dm_StringAt(s, cur, {"GN","KN","PN","AE","WR"}))
                ++cur;

            // Initial vowel maps to 'A'.
            if (cur < length && dm_IsVowel(s[cur])) {
                primary += 'A';
                ++cur;
            }

            auto add = [&](char c) { primary += c; };

            while (cur < length) {
                if (cur >= length) break;
                const char ch = s[cur];

                switch (ch) {
                // ------------------------------------------------------------------
                case 'B':
                    add('P');
                    cur += (s[cur + 1] == 'B') ? 2 : 1;
                    break;

                // ------------------------------------------------------------------
                case 'C':
                    if (cur > 2 && !dm_IsVowel(s[cur-2]) &&
                        dm_StringAt(s, cur-1, {"ACH"}) &&
                        s[cur+2] != 'I' &&
                        (s[cur+2] != 'E' || dm_StringAt(s, cur-2, {"BACHER","MACHER"}))) {
                        add('K'); cur += 2; break;
                    }
                    if (cur == 2 && dm_StringAt(s, cur, {"CAESAR"})) {
                        add('S'); cur += 2; break;
                    }
                    if (dm_StringAt(s, cur, {"CHIA"})) {
                        add('K'); cur += 2; break;
                    }
                    if (dm_StringAt(s, cur, {"CH"})) {
                        if (cur > 2 && dm_StringAt(s, cur-2, {"MAKE","LAKE","MIKE"})) {
                            add('K'); cur += 2; break;
                        }
                        if (dm_StringAt(s, 2, {"HARAC","HARIS"}) ||
                            dm_StringAt(s, 2, {"HOR","HYM","HIA","HEM"})) {
                            add('K'); cur += 2; break;
                        }
                        if (dm_StringAt(s, 2, {"VAN ","VON "}) ||
                            dm_StringAt(s, 2, {"SCH"})) {
                            add('K'); cur += 2; break;
                        }
                        if (dm_StringAt(s, cur-2, {"ORCHES","ARCHIT","ORCHID"}) ||
                            dm_StringAt(s, cur+2, {"T","S"}) ||
                            ((dm_StringAt(s, cur-1, {"A","O","U","E"})) &&
                             dm_StringAt(s, cur+2, {"L","R","N","M","B","H","F","V","W"," "}))) {
                            add('K');
                        } else {
                            add(cur > 2 ? 'X' : 'S');
                        }
                        cur += 2; break;
                    }
                    if (dm_StringAt(s, cur, {"CZ"}) && !dm_StringAt(s, cur-2, {"WICZ"})) {
                        add('S'); cur += 2; break;
                    }
                    if (dm_StringAt(s, cur+1, {"CIA"})) {
                        add('X'); cur += 3; break;
                    }
                    if (dm_StringAt(s, cur, {"CC"}) && !(cur == 3 && s[2] == 'M')) {
                        if (dm_StringAt(s, cur+2, {"I","E","H"})) {
                            if (dm_StringAt(s, cur+2, {"HU"})) {
                                add('K'); cur += 3;
                            } else {
                                add('X'); cur += 3;
                            }
                            break;
                        }
                        add('K'); cur += 2; break;
                    }
                    if (dm_StringAt(s, cur, {"CK","CG","CQ"})) {
                        add('K'); cur += 2; break;
                    }
                    if (dm_StringAt(s, cur, {"CI","CE","CY"})) {
                        if (dm_StringAt(s, cur, {"CIO","CIE","CIA"})) add('X'); else add('S');
                        cur += 2; break;
                    }
                    add('K');
                    if (dm_StringAt(s, cur+1, {"Q","K","C"})) cur += 3; else ++cur;
                    break;

                // ------------------------------------------------------------------
                case 'D':
                    if (dm_StringAt(s, cur, {"DG"})) {
                        if (dm_StringAt(s, cur+2, {"I","E","Y"})) {
                            add('J'); cur += 3;
                        } else {
                            add('T'); add('K'); cur += 2;
                        }
                        break;
                    }
                    if (dm_StringAt(s, cur, {"DT","DD"})) {
                        add('T'); cur += 2; break;
                    }
                    add('T'); ++cur;
                    break;

                // ------------------------------------------------------------------
                case 'F':
                    add('F');
                    cur += (s[cur+1] == 'F') ? 2 : 1;
                    break;

                // ------------------------------------------------------------------
                case 'G':
                    if (s[cur+1] == 'H') {
                        if (cur > 2 && !dm_IsVowel(s[cur-1])) {
                            add('K'); cur += 2; break;
                        }
                        if (cur == 2) {
                            add(s[cur-1] == 'I' ? 'J' : 'K');
                            cur += 2; break;
                        }
                        if ((cur > 4 && dm_IsVowel(s[cur-4])) ||
                            dm_StringAt(s, cur-3, {"IER"})) {
                            cur += 2; break;
                        }
                        if (cur > 3 && dm_IsVowel(s[cur-2]) &&
                            dm_StringAt(s, cur-4, {"HAAG","HAAS","HAAR","MAAS"})) {
                            cur += 2; break;
                        }
                        cur += 2; break;
                    }
                    if (s[cur+1] == 'N') {
                        if (cur == 2 && dm_IsVowel(s[2])) {
                            add('K'); add('N');
                        } else {
                            if (!dm_StringAt(s, cur+2, {"EY"}) && s[cur+1] != 'Y') {
                                add('N');
                            } else {
                                add('K'); add('N');
                            }
                        }
                        cur += 2; break;
                    }
                    if (dm_StringAt(s, cur-1, {"GLI"})) {
                        add('K'); add('L'); ++cur; break;
                    }
                    if (cur == 2 &&
                        (s[cur-1] == 'Y' ||
                         dm_StringAt(s, cur-1, {"ES","EP","EB","EL","EY","IB","IL","IN","IE","EI","ER"}))) {
                        add('K');
                        if (dm_StringAt(s, cur-1, {"ER","EY"})) add('K');
                        ++cur; break;
                    }
                    if (dm_StringAt(s, cur+1, {"E","I","Y"}) ||
                        dm_StringAt(s, cur-1, {"AGGI","OGGI"})) {
                        if (dm_StringAt(s, 2, {"VAN ","VON "}) ||
                            dm_StringAt(s, 2, {"SCH"}) ||
                            dm_StringAt(s, cur+1, {"IER"})) {
                            add('K');
                        } else {
                            add('J');
                        }
                        cur += 2; break;
                    }
                    cur += (s[cur+1] == 'G') ? 2 : 1;
                    add('K');
                    break;

                // ------------------------------------------------------------------
                case 'H':
                    if (dm_IsVowel(s[cur+1]) && (cur == 2 || dm_IsVowel(s[cur-1]))) {
                        add('H');
                        cur += 2;
                    } else {
                        ++cur;
                    }
                    break;

                // ------------------------------------------------------------------
                case 'J':
                    if (dm_StringAt(s, cur, {"JOSE"}) || dm_StringAt(s, 2, {"SAN "})) {
                        if ((cur == 2 && s[cur+3] == ' ') || dm_StringAt(s, 2, {"SAN "}))
                            add('H');
                        else
                            add('J');
                        ++cur; break;
                    }
                    if (cur == 2 && !dm_StringAt(s, cur, {"JOSE"})) {
                        add('J'); ++cur; break;
                    }
                    if (!dm_IsVowel(s[cur-1]) && (s[cur+1] == 'A' || s[cur+1] == 'O')) {
                        add('J');
                    } else {
                        if (cur >= length - 1) {
                            add('J');
                        } else if (!dm_StringAt(s, cur+1, {"L","T","K","S","N","M","B","Z"}) &&
                                   !dm_StringAt(s, cur-1, {"S","K","L"})) {
                            add('J');
                        }
                    }
                    cur += (s[cur+1] == 'J') ? 2 : 1;
                    break;

                // ------------------------------------------------------------------
                case 'K':
                    add('K');
                    cur += (s[cur+1] == 'K') ? 2 : 1;
                    break;

                // ------------------------------------------------------------------
                case 'L':
                    if (s[cur+1] == 'L') {
                        if ((cur == static_cast<int>(s.size()) - 3 &&
                             dm_StringAt(s, cur-1, {"ILLO","ILLA","ALLE"})) ||
                            ((dm_StringAt(s, static_cast<int>(s.size())-2, {"AS","OS"}) ||
                              dm_StringAt(s, static_cast<int>(s.size())-1, {"A","O"})) &&
                             dm_StringAt(s, cur-1, {"ALLE"}))) {
                            add('L'); cur += 2; break;
                        }
                        cur += 2;
                    } else {
                        ++cur;
                    }
                    add('L');
                    break;

                // ------------------------------------------------------------------
                case 'M':
                    if ((dm_StringAt(s, cur-1, {"UMB"}) &&
                         (cur + 1 == length - 1 || dm_StringAt(s, cur+2, {"ER"}))) ||
                        s[cur+1] == 'M')
                        cur += 2;
                    else
                        ++cur;
                    add('M');
                    break;

                // ------------------------------------------------------------------
                case 'N':
                    add('N');
                    cur += (s[cur+1] == 'N') ? 2 : 1;
                    break;

                // ------------------------------------------------------------------
                case 'P':
                    if (s[cur+1] == 'H') {
                        add('F'); cur += 2; break;
                    }
                    add('P');
                    cur += (dm_StringAt(s, cur+1, {"P","B"})) ? 2 : 1;
                    break;

                // ------------------------------------------------------------------
                case 'Q':
                    add('K');
                    cur += (s[cur+1] == 'Q') ? 2 : 1;
                    break;

                // ------------------------------------------------------------------
                case 'R':
                    add('R');
                    cur += (s[cur+1] == 'R') ? 2 : 1;
                    break;

                // ------------------------------------------------------------------
                case 'S':
                    if (dm_StringAt(s, cur-1, {"ISL","YSL"})) { ++cur; break; }
                    if (cur == 2 && dm_StringAt(s, cur, {"SUGAR"})) {
                        add('X'); ++cur; break;
                    }
                    if (dm_StringAt(s, cur, {"SH"})) { add('X'); cur += 2; break; }
                    if (dm_StringAt(s, cur, {"SIO","SIA"})) { add('X'); cur += 3; break; }
                    if (dm_StringAt(s, cur, {"SCH"})) {
                        add('S'); add('K'); cur += 3; break;
                    }
                    if ((cur == 2 && dm_StringAt(s, cur, {"SM","SN","SL","SW"}))) {
                        add('S'); cur += 2; break;
                    }
                    if (dm_StringAt(s, cur, {"SC"})) {
                        if (s[cur+2] == 'H' ||
                            dm_StringAt(s, cur+2, {"OO","ER","EN","UY","ED","EM"})) {
                            add('S'); add('K'); cur += 3; break;
                        }
                        if (dm_StringAt(s, cur+2, {"I","E","Y"})) {
                            add('S'); cur += 3; break;
                        }
                        add('S'); add('K'); cur += 3; break;
                    }
                    if (cur == length-1 && dm_StringAt(s, cur-2, {"AI","OI"})) {
                        // silent
                    } else {
                        add('S');
                    }
                    cur += (dm_StringAt(s, cur+1, {"S","Z"})) ? 2 : 1;
                    break;

                // ------------------------------------------------------------------
                case 'T':
                    if (dm_StringAt(s, cur, {"TION","TIA","TCH"})) {
                        add('X'); cur += 3; break;
                    }
                    if (dm_StringAt(s, cur, {"TH"}) || dm_StringAt(s, cur, {"TTH"})) {
                        // TH -> silent (dental fricative maps to 0 in primary code)
                        cur += 2; break;
                    }
                    add('T');
                    cur += (dm_StringAt(s, cur+1, {"T","D"})) ? 2 : 1;
                    break;

                // ------------------------------------------------------------------
                case 'V':
                    add('F');
                    cur += (s[cur+1] == 'V') ? 2 : 1;
                    break;

                // ------------------------------------------------------------------
                case 'W':
                    if (dm_StringAt(s, cur, {"WR"})) { add('R'); cur += 2; break; }
                    if (cur == 2 && (dm_IsVowel(s[cur+1]) || dm_StringAt(s, cur, {"WH"}))) {
                        add('A');
                    }
                    if ((cur == length-1 && dm_IsVowel(s[cur-1])) ||
                        dm_StringAt(s, cur-1, {"EWSKI","EWSKY","OWSKI","OWSKY"}) ||
                        dm_StringAt(s, 2, {"SCH"})) {
                        add('F'); ++cur; break;
                    }
                    if (dm_StringAt(s, cur, {"WICZ","WITZ"})) {
                        add('T'); add('S'); cur += 4; break;
                    }
                    ++cur;
                    break;

                // ------------------------------------------------------------------
                case 'X':
                    if (!(cur == length-1 &&
                          (dm_StringAt(s, cur-3, {"IAU","EAU"}) ||
                           dm_StringAt(s, cur-2, {"AU","OU"})))) {
                        add('K'); add('S');
                    }
                    cur += (dm_StringAt(s, cur+1, {"X","C"})) ? 2 : 1;
                    break;

                // ------------------------------------------------------------------
                case 'Z':
                    if (s[cur+1] == 'H') { add('J'); cur += 2; break; }
                    if (dm_StringAt(s, cur+1, {"ZO","ZI","ZA"}) ||
                        (cur > 2 && s[cur-1] != 'T')) {
                        add('S');
                    } else {
                        add('S');
                    }
                    cur += (s[cur+1] == 'Z') ? 2 : 1;
                    break;

                // ------------------------------------------------------------------
                default:
                    ++cur;
                    break;
                }
            }

            return primary;
        }

        // -----------------------------------------------------------------------
        // Phonetic similarity via Double Metaphone + word-level Jaccard.
        //
        // Each string is split into words; we compute the DM primary code for
        // each word, then Jaccard(multiset_A, multiset_B) on the resulting code
        // sets.  This handles:
        //   "fire bolt" vs "firebolt"  (same code set regardless of tokenization)
        //   "natronac"  vs "atronach"  (very similar DM codes)
        //   "spent"     vs "sprint"    (both -> SPRNT / SPRT)
        // -----------------------------------------------------------------------
        inline double PhoneticSim(const std::string& a, const std::string& b)
        {
            // Build the DM code multiset for a string
            auto buildSet = [](const std::string& str) {
                std::vector<std::string> codes;
                for (const auto& tok : Tokenize(str)) {
                    std::string code = DoubleMetaphone(tok);
                    if (!code.empty()) codes.push_back(std::move(code));
                }
                std::sort(codes.begin(), codes.end());
                return codes;
            };

            auto sa = buildSet(a);
            auto sb = buildSet(b);

            if (sa.empty() && sb.empty()) return 1.0;
            if (sa.empty() || sb.empty()) return 0.0;

            // Sorted multiset intersection size
            std::size_t inter = 0;
            std::size_t ia = 0, ib = 0;
            while (ia < sa.size() && ib < sb.size()) {
                if (sa[ia] == sb[ib]) { ++inter; ++ia; ++ib; }
                else if (sa[ia] < sb[ib]) ++ia;
                else ++ib;
            }
            const std::size_t unionSize = sa.size() + sb.size() - inter;
            return static_cast<double>(inter) / static_cast<double>(unionSize);
        }

        // -----------------------------------------------------------------------
        // Anchor bonus: fraction of transcript words that appear (exact) in the
        // candidate, weighted by how close the word counts are.
        // -----------------------------------------------------------------------
        inline double AnchorBonus(const std::string& transcript,
                                   const std::string& candidate)
        {
            const auto toks  = Tokenize(transcript);
            const auto ctoks = Tokenize(candidate);
            if (toks.empty()) return 0.0;

            // Sort candidate tokens for binary-search counting
            std::vector<std::string> cSorted(ctoks.begin(), ctoks.end());
            std::sort(cSorted.begin(), cSorted.end());

            std::size_t matched = 0;
            for (const auto& w : toks) {
                auto lo = std::lower_bound(cSorted.begin(), cSorted.end(), w);
                auto hi = std::upper_bound(cSorted.begin(), cSorted.end(), w);
                if (lo != hi) ++matched;
            }

            // Word-count proximity bonus (1 if same count, 0 if wildly different)
            const double nT = static_cast<double>(toks.size());
            const double nC = static_cast<double>(ctoks.size());
            const double wordRatio = (toks.size() == ctoks.size())
                ? 1.0
                : 1.0 - (nT > nC ? nT - nC : nC - nT) / (nT > nC ? nT : nC);

            return (static_cast<double>(matched) / nT) * wordRatio;
        }

        // -----------------------------------------------------------------------
        // Combined score
        // -----------------------------------------------------------------------
        inline double Score(const std::string& transcript, const std::string& candidate)
        {
            // Raw channels — the dominant signal for genuine matches.
            const double levRaw  = LevenshteinSim(transcript, candidate);
            const double phonRaw = PhoneticSim(transcript, candidate);
            const double anch    = AnchorBonus(transcript, candidate);

            // Confusable-folded channels — rescue the ASR's labial-consonant swaps
            // (f/v/b/p/w) AND run-together multi-word transcriptions. Run on the
            // DESPACED folded copies so "pusrodah" ~ "fus ro dah". Separate weighted
            // terms so a folded near-collision can't outrank a true match that already
            // scores high on the raw (space-aware) channels.
            const std::string ft = Despace(FoldConfusables(transcript));
            const std::string fc = Despace(FoldConfusables(candidate));
            const double levFold  = LevenshteinSim(ft, fc);
            const double phonFold = PhoneticSim(ft, fc);

            constexpr double kLevRaw   = 0.25;
            constexpr double kPhonRaw  = 0.15;
            constexpr double kLevFold  = 0.25;
            constexpr double kPhonFold = 0.20;
            constexpr double kAnchor   = 0.15;

            return kLevRaw  * levRaw  + kPhonRaw  * phonRaw +
                   kLevFold * levFold + kPhonFold * phonFold +
                   kAnchor  * anch;
        }

    }  // namespace detail

    // =========================================================================
    // Public API
    // =========================================================================

    // Returns the best-matching candidate and its score in [0, 1].
    // If candidates is empty, returns {"", 0.0}.
    // Safe to call from any thread; O(|transcript| * |candidates|) per call.
    inline FuzzyResult BestFuzzyMatch(
        const std::string&              transcript,
        const std::vector<std::string>& candidates)
    {
        if (candidates.empty()) return {"", 0.0};

        const std::string normT = detail::Normalize(transcript);

        FuzzyResult best{"", -1.0};
        for (const auto& cand : candidates) {
            const std::string normC = detail::Normalize(cand);
            const double s = detail::Score(normT, normC);
            if (s > best.score) {
                best.score  = s;
                best.phrase = cand;
            }
        }
        return best;
    }

}  // namespace VSC
