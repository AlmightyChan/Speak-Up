#include "PCH.h"
#include "VoiceController.h"
#include "Equipper.h"   // for g_injectingShoutKey
#include "SherpaRecognizer.h"
#include "Vocabulary.h"
#include "PhraseNormalize.h"
#include "GlobalCommands.h"
#include "FuzzyMatch.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

namespace logger = SKSE::log;

namespace VSC
{
    namespace
    {
        // Corner notification (marshaled to the main thread — callers are on input/mic threads).
        // Takes std::string (not const char*) so the lambda captures by value and the
        // string lifetime is not tied to a raw pointer that may be a temporary.
        void Notify(std::string a_msg)
        {
            if (auto* task = SKSE::GetTaskInterface()) {
                task->AddTask([msg = std::move(a_msg)]() { RE::DebugNotification(msg.c_str()); });
            }
        }

        class SpellLearnSink : public RE::BSTEventSink<RE::SpellsLearned::Event>
        {
        public:
            static SpellLearnSink* GetSingleton()
            {
                static SpellLearnSink s;
                return &s;
            }
            RE::BSEventNotifyControl ProcessEvent(
                const RE::SpellsLearned::Event*,
                RE::BSTEventSource<RE::SpellsLearned::Event>*) override
            {
                if (auto* task = SKSE::GetTaskInterface()) {
                    task->AddTask([]() { VoiceController::Get().RefreshGrammar(true); });  // force
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        // Vampire Lord / Werewolf transforms (and reverting) swap the player's race +
        // ability set but fire NO SpellsLearned event — so without this they'd only be
        // caught by the slow poll. TESSwitchRaceCompleteEvent fires on the player the
        // moment the race swap finishes (both directions), so we refresh immediately.
        class RaceSwitchSink : public RE::BSTEventSink<RE::TESSwitchRaceCompleteEvent>
        {
        public:
            static RaceSwitchSink* GetSingleton()
            {
                static RaceSwitchSink s;
                return &s;
            }
            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESSwitchRaceCompleteEvent* a_event,
                RE::BSTEventSource<RE::TESSwitchRaceCompleteEvent>*) override
            {
                if (a_event && a_event->subject && a_event->subject->IsPlayerRef()) {
                    if (auto* task = SKSE::GetTaskInterface()) {
                        task->AddTask([]() {
                            logger::info("[voice] race switch (transform) — refreshing grammar");
                            VoiceController::Get().RefreshGrammar(true);  // force
                        });
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        // Fire a (diff-gated) refresh whenever ANY menu CLOSES. This does two jobs at
        // once, both cheap:
        //   1) Re-applies config the instant you exit the MCM — MCM Helper writes its
        //      settings INI on menu close, so logging/sensitivity/keybinds take effect
        //      immediately instead of on the next 3s tick or a reload (RefreshGrammar
        //      calls LoadConfig first, which is mtime-gated so it's a no-op if unchanged).
        //   2) Catches a word-of-power being unlocked (the unlock UI is in the Magic Menu),
        //      via the cheap shout-signature diff. If nothing changed, it's a no-op.
        class MenuCloseSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
        {
        public:
            static MenuCloseSink* GetSingleton()
            {
                static MenuCloseSink s;
                return &s;
            }
            RE::BSEventNotifyControl ProcessEvent(
                const RE::MenuOpenCloseEvent* a_event,
                RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
            {
                if (a_event && !a_event->opening) {
                    if (auto* task = SKSE::GetTaskInterface()) {
                        task->AddTask([]() { VoiceController::Get().RefreshGrammar(); });  // diff + config
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        // Second, redundant-but-cheap path: the misc-stat bump when a word/shout is
        // learned or unlocked. Also catches console teachword/unlockword (which bumps the
        // stat). Diff-gated refresh, so harmless if it fires spuriously.
        class StatsSink : public RE::BSTEventSink<RE::TESTrackedStatsEvent>
        {
        public:
            static StatsSink* GetSingleton()
            {
                static StatsSink s;
                return &s;
            }
            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESTrackedStatsEvent* a_event,
                RE::BSTEventSource<RE::TESTrackedStatsEvent>*) override
            {
                if (a_event && a_event->stat.c_str()) {
                    const std::string_view s = a_event->stat.c_str();
                    if (s == "Words Of Power Unlocked" || s == "Words Of Power Learned" ||
                        s == "Shouts Learned") {
                        if (auto* task = SKSE::GetTaskInterface()) {
                            task->AddTask([]() { VoiceController::Get().RefreshGrammar(); });  // diff
                        }
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        // ---- Listen controls (all DX scan codes; 0 = unbound). Two independent
        // bindings, each with an OPTIONAL modifier: a persistent TOGGLE and a
        // momentary PUSH-TO-TALK. Default unbound -> always listening (plug-and-play).
        std::atomic<std::uint32_t> g_toggleKey{ 0 }, g_toggleMod{ 0 };
        std::atomic<std::uint32_t> g_pttKey{ 0 },    g_pttMod{ 0 };
        std::atomic<bool>          g_toggleOn{ true };   // persistent listen state
        std::atomic<bool>          g_pttHeld{ false };   // momentary (while PTT held)

        // Held state of every keyboard DX code, so a binding can require a modifier and a
        // bare (no-modifier) binding can require that NO modifier is held (this is what
        // lets "T" = push-to-talk and "Alt+T" = toggle coexist on the same key).
        std::array<std::atomic<bool>, 512> g_keyDown{};
        constexpr std::uint32_t kStdMods[] = { 0x38, 0xB8, 0x1D, 0x9D, 0x2A, 0x36 };  // L/R Alt,Ctrl,Shift
        bool AnyModDown() { for (auto m : kStdMods) if (g_keyDown[m].load()) return true; return false; }
        bool KeyHeld(std::uint32_t c) { return c < g_keyDown.size() && g_keyDown[c].load(); }
        // A binding fires when its key is pressed AND its modifier condition holds:
        // modifier set -> that modifier must be held; modifier none -> NO modifier held.
        bool ModSatisfied(std::uint32_t mod) { return mod == 0 ? !AnyModDown() : KeyHeld(mod); }

        bool EffectiveListening() { return g_toggleOn.load() || g_pttHeld.load(); }

        class ListenHotkeySink : public RE::BSTEventSink<RE::InputEvent*>
        {
        public:
            static ListenHotkeySink* GetSingleton()
            {
                static ListenHotkeySink s;
                return &s;
            }
            RE::BSEventNotifyControl ProcessEvent(
                RE::InputEvent* const* a_event,
                RE::BSTEventSource<RE::InputEvent*>*) override
            {
                if (!a_event) return RE::BSEventNotifyControl::kContinue;

                // If CastShoutNow is currently injecting the Shout key via SendInput,
                // skip ALL PTT/toggle processing for this event chain.  This prevents
                // the synthetic key from toggling push-to-talk listening when PTT happens
                // to be bound to the same key as Shout (common for Space-bar setups).
                if (VSC::g_injectingShoutKey.load()) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                for (auto* e = *a_event; e; e = e->next) {
                    auto* btn = e->AsButtonEvent();
                    if (!btn || e->GetDevice() != RE::INPUT_DEVICE::kKeyboard) continue;
                    const std::uint32_t c = btn->GetIDCode();
                    if (c < g_keyDown.size()) g_keyDown[c].store(!btn->IsUp());  // track held state

                    const std::uint32_t pttKey = g_pttKey.load();
                    if (pttKey && c == pttKey) {
                        if (btn->IsDown() && ModSatisfied(g_pttMod.load())) {
                            if (!g_pttHeld.exchange(true)) {
                                logger::info("[voice] push-to-talk engaged");
                                Notify("Speak Up: listening...");
                            }
                        } else if (btn->IsUp()) {
                            if (g_pttHeld.exchange(false)) {
                                logger::info("[voice] push-to-talk released");
                                VoiceController::Get().OnPushToTalkReleased();
                            }
                        }
                    }
                    const std::uint32_t tKey = g_toggleKey.load();
                    if (tKey && c == tKey && btn->IsDown() && ModSatisfied(g_toggleMod.load())) {
                        VoiceController::Get().ToggleListening();
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
    }

    VoiceController& VoiceController::Get()
    {
        static VoiceController singleton;
        return singleton;
    }

    namespace
    {
        // Two config sources, in priority order:
        //   1) the MCM Helper settings INI (written live by the in-game MCM), and
        //   2) our shipped SKSE-plugin INI (defaults; also the config for users who
        //      don't install the MCM ESP).
        // Each key is read from the MCM file first and falls back to the SKSE file,
        // so the mod is fully functional with OR without the MCM installed.
        constexpr const char* kIni    = "Data\\SKSE\\Plugins\\SpeakUp.ini";
        constexpr const char* kMcmIni = "Data\\MCM\\Settings\\SpeakUp.ini";

        // Parsed [Voice] section of the MCM settings file (refreshed in LoadConfig).
        std::unordered_map<std::string, std::string> g_mcm;

        std::string TrimWs(const std::string& s)
        {
            const auto a = s.find_first_not_of(" \t");
            if (a == std::string::npos) return {};
            const auto b = s.find_last_not_of(" \t");
            return s.substr(a, b - a + 1);
        }

        // Parse the MCM settings file's [Voice] section ourselves. We do NOT use
        // GetPrivateProfileString for this file because MCM Helper writes it with a
        // UTF-8 BOM, which makes the Win32 INI parser fail to match the FIRST section —
        // so every MCM value was silently falling back to defaults (push-to-talk read as
        // unbound, debug logging stuck off, etc.). We strip the BOM and trim the spaces
        // MCM Helper puts around '=' ("bDebugLogging = 1").
        void LoadMcmValues()
        {
            g_mcm.clear();
            std::ifstream f(kMcmIni, std::ios::binary);
            if (!f) return;
            std::string line;
            bool inVoice = false, first = true;
            while (std::getline(f, line)) {
                if (first) {  // strip a UTF-8 BOM on the first line
                    if (line.size() >= 3 && (unsigned char)line[0] == 0xEF &&
                        (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) {
                        line.erase(0, 3);
                    }
                    first = false;
                }
                if (!line.empty() && line.back() == '\r') line.pop_back();
                std::string t = TrimWs(line);
                if (t.empty() || t[0] == ';') continue;
                if (t[0] == '[') { inVoice = (t == "[Voice]"); continue; }
                if (!inVoice) continue;
                const auto eq = t.find('=');
                if (eq == std::string::npos) continue;
                g_mcm[TrimWs(t.substr(0, eq))] = TrimWs(t.substr(eq + 1));
            }
        }

        // Read a [Voice] key: MCM settings (parsed above) first, then the shipped SKSE
        // INI (no BOM there — read normally), else the default. a_mcmKey is MCM Helper's
        // type-prefixed name (e.g. "bInstantCast"); a_iniKey is the unprefixed SKSE name.
        std::string ReadCfg(const char* a_mcmKey, const char* a_iniKey, const char* def)
        {
            if (auto it = g_mcm.find(a_mcmKey); it != g_mcm.end()) return it->second;
            char buf[64]{};
            ::GetPrivateProfileStringA("Voice", a_iniKey, def, buf, sizeof(buf), kIni);
            return buf;
        }

        bool ReadCfgBool(const char* a_mcmKey, const char* a_iniKey, bool def)
        {
            std::string v = ReadCfg(a_mcmKey, a_iniKey, def ? "1" : "0");
            return !v.empty() && v != "0" && v != "false" && v != "False";
        }

        Hand ParseHand(const char* s, Hand fallback)
        {
            std::string h = s ? s : "";
            std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (h == "right" || h == "1") return Hand::Right;
            if (h == "left"  || h == "0") return Hand::Left;
            if (h == "both" || h == "dual" || h == "2") return Hand::Both;
            return fallback;
        }

        // Dovahzul word -> an ENGLISH re-spelling whose grapheme-to-phoneme guess is closer
        // to how the word is actually SPOKEN, so the open-vocab speech model is more likely
        // to transcribe a spoken Word of Power to a string we match. We feed the recognizer
        // TEXT (not phonemes), so a g2p-friendly respelling is how we nudge it.
        //
        // This is a single CONSISTENT rule derived from the full vanilla+DLC Word-of-Power
        // set: Dovahzul pronunciation is regular, so the same rule auto-covers MODDED shouts
        // with no per-word table to maintain. Returns "" when the rule changes nothing (the
        // raw romanization is already a fine candidate, e.g. "dah", "kest", "toor").
        std::string DovahRespell(const std::string& w)
        {
            std::string s;
            s.reserve(w.size() + 4);
            for (char c : w) s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            const std::string orig = s;

            auto repl = [&s](const char* from, const char* to) {
                const std::string f = from, t = to;
                std::size_t p = 0;
                while ((p = s.find(f, p)) != std::string::npos) { s.replace(p, f.size(), t); p += t.size(); }
            };

            // 1) Long-vowel digraphs -> their English-sounding form.
            repl("aa", "ah"); repl("uu", "oo"); repl("ii", "ee");
            repl("ei", "ay"); repl("ey", "ay"); repl("au", "ow");
            // 2) The 'ir' vowel (dragon-font rune 7, e.g. Nir/Mir) -> "eer" (after 'ii').
            repl("ir", "eer");
            // 3) Single 'u' -> "oo" (Fus->foos, Su->soo); this can create new "oo".
            repl("u", "oo");
            // 4) Single 'o' -> "oh" (Ro->roh, Lok->lohk) WITHOUT mangling the "oo" sound:
            //    park every "oo" on a sentinel, rewrite 'o', then restore.
            repl("oo", "\x01"); repl("o", "oh"); repl("\x01", "oo");
            // 5) Consonants the ASR reads differently than Dovahzul speaks them.
            repl("j", "y");   // Joor -> "yoor"
            repl("q", "k");   // Qo   -> "koh"

            return s == orig ? std::string{} : s;  // empty = raw form already fine
        }

        // Spoken number words for 1..24 (index = value; [0] unused). Used to build the
        // "wait N hours" grammar phrases and to parse a recognized one back to a count.
        const std::array<const char*, 25>& NumberWords()
        {
            static const std::array<const char*, 25> w = { "",
                "one","two","three","four","five","six","seven","eight","nine","ten",
                "eleven","twelve","thirteen","fourteen","fifteen","sixteen","seventeen",
                "eighteen","nineteen","twenty","twenty one","twenty two","twenty three",
                "twenty four" };
            return w;
        }

        // Parse a recognized "wait <number> hour[s]" phrase -> hours (1..24); else 0.
        int ParseWaitHours(const std::string& phrase)
        {
            if (phrase.rfind("wait ", 0) != 0) return 0;
            std::string rest = phrase.substr(5);
            for (const char* suf : { " hours", " hour" }) {
                std::string s = suf;
                if (rest.size() > s.size() && rest.compare(rest.size() - s.size(), s.size(), s) == 0) {
                    std::string num = rest.substr(0, rest.size() - s.size());
                    for (int i = 1; i <= 24; ++i) {
                        if (num == NumberWords()[i]) return i;
                    }
                    char* end = nullptr;  // also accept a bare digit form
                    long v = std::strtol(num.c_str(), &end, 10);
                    if (end && *end == '\0' && v >= 1 && v <= 24) return static_cast<int>(v);
                    return 0;
                }
            }
            return 0;
        }
    }

    void VoiceController::LoadConfig()
    {
        // The ticker calls this ~every 3s so in-game MCM/INI changes apply without a
        // restart. Re-reading ~15 INI keys (each up to two GetPrivateProfileString file
        // reads, through MO2's VFS) every tick is wasteful, so gate on the INI files'
        // last-write time: if neither file changed since the last successful load, skip.
        {
            std::error_code ec1, ec2;
            auto mMcm = std::filesystem::last_write_time(kMcmIni, ec1);
            auto mIni = std::filesystem::last_write_time(kIni, ec2);
            const bool statsOk = !ec1 || !ec2;  // at least one file present/statable
            static std::filesystem::file_time_type s_lastMcm{}, s_lastIni{};
            static bool s_loadedOnce = false;
            if (s_loadedOnce && statsOk && mMcm == s_lastMcm && mIni == s_lastIni) {
                return;  // nothing changed — keep cached config, no re-read
            }
            s_loadedOnce = true;
            s_lastMcm = mMcm;
            s_lastIni = mIni;
        }

        // Parse the (BOM'd) MCM settings file ourselves so its [Voice] values are read.
        LoadMcmValues();

        // Re-read on first load / whenever an INI changed. We only LOG on actual changes.
        // iDefaultAction (MCM enum: 0=Equip, 1=Cast) supersedes the old bDefaultActionCast
        // toggle. Back-compat: if iDefaultAction is absent from both INIs (old installs),
        // fall back to the bool bDefaultActionCast / DefaultActionCast.
        {
            bool mcmHasNewKey = (g_mcm.find("iDefaultAction") != g_mcm.end());
            char iniBuf[64]{};
            ::GetPrivateProfileStringA("Voice", "DefaultAction", "__absent__", iniBuf, sizeof(iniBuf), kIni);
            bool iniHasNewKey = (std::string(iniBuf) != "__absent__");
            if (mcmHasNewKey || iniHasNewKey) {
                // New key present: 0=Equip, 1=Cast; default Cast (1).
                long v = std::strtol(ReadCfg("iDefaultAction", "DefaultAction", "1").c_str(), nullptr, 10);
                _defaultCast = (v != 0);
            } else {
                // Legacy back-compat: read the old bool key.
                _defaultCast = ReadCfgBool("bDefaultActionCast", "DefaultActionCast", true);
            }
        }

        // EquipDefaultHand accepts a word ("Left"/"Right"/"Both") from the SKSE INI
        // or an enum index ("0"/"1"/"2") from the MCM's dropdown — ParseHand maps both.
        _equipHand = ParseHand(ReadCfg("iEquipDefaultHand", "EquipDefaultHand", "Left").c_str(), Hand::Left);

        std::string thr = ReadCfg("fLongCastThreshold", "LongCastThreshold", "1.0");

        // Listen controls: a persistent TOGGLE and a momentary PUSH-TO-TALK, each with an
        // optional modifier. MCM keymaps store -1 when unbound; treat <=0 as unbound.
        auto readKey = [&](const char* mcm, const char* ini) -> std::uint32_t {
            long v = std::strtol(ReadCfg(mcm, ini, "0").c_str(), nullptr, 0);  // hex (0x..) or dec
            return v > 0 ? static_cast<std::uint32_t>(v) : 0u;
        };
        g_toggleKey.store(readKey("iListenToggleKeyDX", "ListenToggleKeyDX"));
        g_toggleMod.store(readKey("iListenToggleModDX", "ListenToggleModDX"));
        g_pttKey.store(readKey("iPushToTalkKeyDX", "PushToTalkKeyDX"));
        g_pttMod.store(readKey("iPushToTalkModDX", "PushToTalkModDX"));
        // Master enables (the MCM grays out the key fields until these are on) — also
        // honored here so a leftover bound key does nothing while its feature is disabled.
        if (!ReadCfgBool("bEnablePushToTalk", "EnablePushToTalk", false)) {
            g_pttKey.store(0); g_pttMod.store(0);
        }
        if (!ReadCfgBool("bEnableListenToggle", "EnableListenToggle", false)) {
            g_toggleKey.store(0); g_toggleMod.store(0);
        }
        // Set the BASE listen state whenever push-to-talk's bound-state changes (and on
        // first load): PTT bound -> start OFF (hold to talk); PTT unbound -> always on.
        // We must re-evaluate on change (not just once), or enabling PTT in the in-game
        // MCM after launch would leave it stuck always-listening. Between changes we leave
        // g_toggleOn alone so a live toggle press isn't stomped.
        const bool pttBound = (g_pttKey.load() != 0);
        if (!_listenDefaultsApplied || pttBound != _pttBoundPrev) {
            _listenDefaultsApplied = true;
            _pttBoundPrev = pttBound;
            g_toggleOn.store(!pttBound);
        }

        // Fuzzy match threshold for the open-vocab transcripts (0..1); default 0.62.
        // This is the live "recognition confidence" knob (applied at HandleResult).
        _sherpaMatchThreshold = std::strtof(
            ReadCfg("fSherpaMatchThreshold", "SherpaMatchThreshold", "0.62").c_str(), nullptr);

        // Endpoint trailing-silence rules ("sensitivity"/responsiveness). Pushed to the
        // recognizer here so the values stay current; they take effect at the next
        // recognizer (re)start (see PushEndpointRules / SherpaRecognizer::SetEndpointRules).
        PushEndpointRules();

        // Scale handling: by default include ALL known spells (the hard cap below is
        // the crash failsafe). When this toggle is ON and the list is huge, scope the
        // grammar to favorited + equipped magic (+ all powers/shouts) — see RefreshGrammar.
        _scopeAtScale = ReadCfgBool("bScopeAtScale", "ScopeAtScale", false);

        // ---- Developer options (default OFF so shipped users store no extra data) ----
        // DebugLogging: when off, the log level is raised to WARN so the high-volume
        // info lines (every recognized phrase, roster/grammar diagnostics) are NOT
        // written. Turn it ON in the MCM only while testing.
        _debugLog    = ReadCfgBool("bDebugLogging", "DebugLogging", true);
        _dumpGrammar = ReadCfgBool("bDumpGrammar", "DumpGrammar", true);
        spdlog::set_level((_debugLog || _dumpGrammar) ? spdlog::level::info : spdlog::level::warn);
        // Dump the live grammar once on the rising edge of the toggle (so a dev can
        // inspect exactly what's recognized without spamming it every refresh tick).
        if (_dumpGrammar && !_dumpGrammarPrev) {
            DumpGrammar();
        }
        _dumpGrammarPrev = _dumpGrammar;

        // bRestartRecognizer (MCM) / RestartRecognizer (INI): EDGE-TRIGGERED.
        // The user flips it ON to request a restart.  We fire once on the rising edge
        // (false -> true) then track the new value.  To restart again, the user turns it
        // off then on again — or it may stay on across sessions (next rising edge fires on
        // the next toggle-off / toggle-on cycle).
        {
            bool restartReq = ReadCfgBool("bRestartRecognizer", "RestartRecognizer", false);
            if (restartReq && !_restartReqPrev) {
                // Rising edge: call RestartRecognizer() directly — LoadConfig already
                // runs on the main thread (AddTask / menu-close path).
                RestartRecognizer();
            }
            _restartReqPrev = restartReq;
        }

        CastSettings cs;
        cs.instantCast        = ReadCfgBool("bInstantCast", "InstantCast", true);
        cs.allowConcentration = ReadCfgBool("bAllowConcentration", "AllowConcentration", false);
        cs.allowLongCast      = ReadCfgBool("bAllowLongCast", "AllowLongCast", false);
        cs.playShoutAnimation = ReadCfgBool("bPlayShoutAnimation", "PlayShoutAnimation", false);
        cs.longCastThreshold  = std::strtof(thr.c_str(), nullptr);
        cs.equipHand          = _equipHand;
        // ShoutUseRealCast=1 (default, dual-path): auto-detect per shout via archetype
        // inspection.  Shouts whose effects need the engine voice pipeline (kScript,
        // kEtherealize, kSlowTime, kSpawnHazard, etc.) get equip+SendInput; simple shouts
        // (Unrelenting Force, Elemental Fury, etc.) get CastSpellImmediate + ShakeCamera.
        // ShoutUseRealCast=0 (legacy): force CastSpellImmediate for ALL shouts (no SendInput).
        cs.shoutUseRealCast = ReadCfgBool("bShoutUseRealCast", "ShoutUseRealCast", true);
        // ShoutPlayVoice (immersion): 0 (default) = SILENT — you spoke the words, so you are
        // the voice; shouts fire their effect with no character Thu'um vocalization. 1 = the
        // character vocalizes the Thu'um (full engine pipeline for every shout).
        cs.shoutPlayVoice = ReadCfgBool("bShoutPlayVoice", "ShoutPlayVoice", false);
        // ShoutKeyDX: DirectInput scan code for the Shout key, used when ControlMap returns
        // unmapped. Default 0x39 = Space (vanilla default shout/sheathe binding).
        {
            long sc = std::strtol(ReadCfg("iShoutKeyDX", "ShoutKeyDX", "0x39").c_str(), nullptr, 0);
            cs.shoutKeyDX = (sc > 0) ? static_cast<std::uint32_t>(sc) : 0x39u;
        }
        SetCastSettings(cs);

        // Log only on first load / when something changed (avoids 3s ticker spam).
        std::string snapshot = std::to_string(_defaultCast) + "|" + HandName(_equipHand) + "|" +
            std::to_string(cs.instantCast) + "|" + std::to_string(cs.allowConcentration) + "|" +
            std::to_string(cs.allowLongCast) + "|" + std::to_string(cs.longCastThreshold) + "|" +
            std::to_string(g_toggleKey.load()) + "+" + std::to_string(g_toggleMod.load()) + "|" +
            std::to_string(g_pttKey.load()) + "+" + std::to_string(g_pttMod.load()) + "|" +
            std::to_string(_scopeAtScale) + "|" + std::to_string(cs.playShoutAnimation) + "|" +
            std::to_string(_sherpaMatchThreshold) + "|" +
            std::to_string(cs.shoutUseRealCast) + "|" + std::to_string(cs.shoutPlayVoice) + "|" +
            std::to_string(cs.shoutKeyDX) + "|" +
            std::to_string(_restartReqPrev);
        if (snapshot != _lastConfigSnapshot) {
            _lastConfigSnapshot = snapshot;
            logger::info("[voice] config: defaultCast={} equipHand={} instantCast={} "
                         "allowConcentration={} allowLongCast={} longThreshold={:.2f} "
                         "toggle=0x{:X}+0x{:X} ptt=0x{:X}+0x{:X} scopeAtScale={} "
                         "matchThreshold={:.2f} "
                         "shoutUseRealCast={} shoutPlayVoice={} shoutKeyDX=0x{:X}",
                _defaultCast, HandName(_equipHand), cs.instantCast, cs.allowConcentration,
                cs.allowLongCast, cs.longCastThreshold, g_toggleKey.load(), g_toggleMod.load(),
                g_pttKey.load(), g_pttMod.load(), _scopeAtScale,
                _sherpaMatchThreshold,
                cs.shoutUseRealCast, cs.shoutPlayVoice, cs.shoutKeyDX);
        }
    }

    void VoiceController::Start()
    {
        if (_started) return;
        _started = true;

        // Read the match threshold + endpoint rules BEFORE starting the in-process
        // recognizer so the very first InitEngine() uses the configured values.
        LoadMcmValues();
        _sherpaMatchThreshold = std::strtof(
            ReadCfg("fSherpaMatchThreshold", "SherpaMatchThreshold", "0.62").c_str(), nullptr);
        PushEndpointRules();

        logger::info("[voice] starting recognizer (open-vocab + fuzzy match, threshold={:.2f})",
            _sherpaMatchThreshold);
        SherpaRecognizer::Get().Start([](const std::string& phrase) {
            VoiceController::Get().OnPhraseRecognized(phrase);
        });
    }

    void VoiceController::RegisterEvents()
    {
        if (auto* src = RE::SpellsLearned::GetEventSource()) {
            src->AddEventSink(SpellLearnSink::GetSingleton());
            logger::info("[voice] SpellsLearned sink registered");
        }
        if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
            holder->AddEventSink<RE::TESSwitchRaceCompleteEvent>(RaceSwitchSink::GetSingleton());
            holder->AddEventSink<RE::TESTrackedStatsEvent>(StatsSink::GetSingleton());
            logger::info("[voice] transform + tracked-stats sinks registered");
        }
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuCloseSink::GetSingleton());
            logger::info("[voice] magic-menu-close sink registered (word-of-power unlock)");
        }
        if (auto* idm = RE::BSInputDeviceManager::GetSingleton()) {
            idm->AddEventSink(ListenHotkeySink::GetSingleton());
            logger::info("[voice] listen control sink registered (toggle 0x{:X}, push-to-talk 0x{:X}; 0=unbound)",
                g_toggleKey.load(), g_pttKey.load());
        }
    }

    void VoiceController::ToggleListening()
    {
        const bool now = !g_toggleOn.load();
        g_toggleOn.store(now);
        logger::info("[voice] listening {} (toggle hotkey)", now ? "ON" : "OFF");
        Notify(now ? "Speak Up: On" : "Speak Up: Off");
    }

    void VoiceController::OnPushToTalkReleased()
    {
        // Flush the tail utterance so a command spoken right before releasing the key
        // still fires (push-to-talk: "hold, speak, release → it sends").
        SherpaRecognizer::Get().Finalize();
    }

    void VoiceController::PushEndpointRules()
    {
        // sherpa endpoint trailing-silence rules (seconds). Defaults mirror the values
        // that were previously hardcoded in SherpaRecognizer::InitEngine. The recognizer
        // clamps to sane bounds, so out-of-range INI values can't wedge recognition.
        const float r1 = std::strtof(ReadCfg("fSherpaEndpointRule1", "SherpaEndpointRule1", "0.7").c_str(),  nullptr);
        const float r2 = std::strtof(ReadCfg("fSherpaEndpointRule2", "SherpaEndpointRule2", "0.45").c_str(), nullptr);
        const float r3 = std::strtof(ReadCfg("fSherpaEndpointRule3", "SherpaEndpointRule3", "20.0").c_str(), nullptr);
        SherpaRecognizer::Get().SetEndpointRules(r1, r2, r3);
    }

    void VoiceController::CheckMicStatus()
    {
        // Edge-triggered so the player sees one notice, not a per-tick spam. The recognizer
        // already retried the mic several times on launch (see SherpaRecognizer::AcquireMic);
        // this just surfaces the final verdict gracefully.
        const bool failed = SherpaRecognizer::Get().MicStartFailed();
        if (failed && !_micFailNotified) {
            _micFailNotified = true;
            logger::warn("[voice] microphone did not start — notifying player");
            Notify("Speak Up: mic couldn't connect. Try 'Restart recognizer' in the MCM "
                   "(Developer), or relaunch.");
        } else if (!failed && _micFailNotified) {
            // Mic recovered (e.g. after a restart) — clear the latch and reassure.
            _micFailNotified = false;
            Notify("Speak Up: microphone connected.");
        }
    }

    void VoiceController::RestartRecognizer()
    {
        // Must run on the main thread (called from LoadConfig via AddTask / menu-close).
        logger::info("[voice] RestartRecognizer: restarting recognizer");
        Notify("Speak Up: restarting recognizer...");
        SherpaRecognizer::Get().Restart();
    }

    void VoiceController::DumpGrammar()
    {
        std::lock_guard<std::mutex> lock(_mapMutex);
        logger::info("[dump] LIVE grammar: {} mapped phrases", _phraseMap.size());
        for (const auto& kv : _phraseMap) {
            const auto& t = kv.second;
            const std::string name = (t.specIndex < _roster.size()) ? _roster[t.specIndex].name : std::string("?");
            logger::info("[dump]   \"{}\" -> {} {} (hand={}{}{})",
                kv.first, ActionName(t.action), name, HandName(t.hand),
                t.dual ? ", dual" : "",
                t.shoutLevel >= 0 ? std::format(", word {}", t.shoutLevel + 1) : std::string{});
        }
        logger::info("[dump] global commands:");
        for (const auto& kv : GlobalCommandPhrases()) {
            logger::info("[dump]   \"{}\" -> global", kv.first);
        }
    }

    void VoiceController::MarkGameReady()
    {
        _gameReady.store(true);
        RefreshGrammar(true);  // force full build on save load
        StartTicker();
    }

    void VoiceController::StartTicker()
    {
        if (_tickerStarted.exchange(true)) return;
        // Lightweight poll for the ONE change with no event: a word of power being
        // unlocked (dragon soul spent). Spells (SpellsLearned) and transforms
        // (TESSwitchRaceCompleteEvent) are event-driven. RefreshGrammar(false) early-outs
        // on a cheap shout-unlock signature, so a tick costs ~a few dozen GetKnown() reads
        // unless a word actually unlocked.
        _ticker = std::thread([this] {
            using namespace std::chrono_literals;
            for (;;) {
                std::this_thread::sleep_for(3s);
                if (!_gameReady.load()) continue;
                if (auto* task = SKSE::GetTaskInterface()) {
                    task->AddTask([]() {
                        VoiceController::Get().CheckMicStatus();  // graceful mic-fail notice
                        VoiceController::Get().RefreshGrammar();
                    });
                }
            }
        });
        _ticker.detach();  // session-lifetime; process exit reclaims
    }

    namespace
    {
        // CHEAP composite signature for the poll's diff check — no full roster build, no
        // string work. Covers: every shout's per-word unlock bits (the main poll job),
        // plus added-spell COUNT, current race (transforms), and selected power. If none
        // of these changed, nothing the poll cares about changed, so we skip entirely.
        // (Events still drive the common cases; this is the safety-net fallback.)
        std::uint64_t ComputeCheapSig()
        {
            std::uint64_t sig = 1469598103934665603ull;
            auto mix = [&sig](std::uint64_t v) { sig ^= v; sig *= 1099511628211ull; };
            if (auto* dh = RE::TESDataHandler::GetSingleton()) {
                for (auto* sh : dh->GetFormArray<RE::TESShout>()) {
                    if (!sh) continue;
                    for (int lvl = 0; lvl < RE::TESShout::VariationIDs::kTotal; ++lvl) {
                        const auto& v = sh->variations[lvl];
                        mix(v.word && v.word->GetKnown() ? (sh->GetFormID() ^ (lvl + 1)) : 0);
                    }
                }
            }
            if (auto* pc = RE::PlayerCharacter::GetSingleton()) {
                auto& rt = pc->GetActorRuntimeData();
                mix(rt.addedSpells.size());                                  // spell count
                mix(pc->GetRace() ? pc->GetRace()->GetFormID() : 0);         // transforms
                mix(rt.selectedPower ? rt.selectedPower->GetFormID() : 0);   // power swap
            }
            return sig;
        }
    }

    void VoiceController::RefreshGrammar(bool a_force)
    {
        if (!_gameReady.load()) {
            logger::info("[voice] grammar refresh skipped — no save loaded yet");
            return;
        }
        LoadConfig();

        // Poll path (a_force=false): skip the whole rebuild unless the cheap composite
        // signature changed. Diff-based, so it never re-runs on its own — it only acts
        // when something it tracks actually changed. Events pass a_force=true.
        std::uint64_t cs = ComputeCheapSig();
        if (!a_force && _shoutSigValid && cs == _shoutSig) {
            return;
        }
        _shoutSig = cs;
        _shoutSigValid = true;

        auto roster = BuildRoster();

        // ---- Optional working-set scoping (OFF by default) ------------------------
        // When enabled AND the spell list is very large, restrict the grammar's SPELLS
        // to a working set (favorited + equipped magic). Powers and shouts are always
        // kept (there are few). This keeps the active vocabulary small -> low memory,
        // fast + accurate recognition no matter how many thousands of spells are known.
        // The hard cap further below is the always-on CRASH failsafe regardless of this.
        constexpr std::size_t kScopeThreshold = 600;
        if (_scopeAtScale && roster.size() > kScopeThreshold) {
            std::unordered_set<RE::FormID> keep;
            if (auto* fav = RE::MagicFavorites::GetSingleton()) {
                for (auto* f : fav->spells) {
                    if (f) keep.insert(f->GetFormID());
                }
            }
            if (auto* pc = RE::PlayerCharacter::GetSingleton()) {
                auto& sel = pc->GetActorRuntimeData().selectedSpells;
                for (auto* m : sel) {
                    if (m) keep.insert(m->GetFormID());
                }
            }
            std::vector<RosterEntry> scoped;
            scoped.reserve(keep.size() + 64);
            for (auto& e : roster) {
                if (e.category != Category::Spell || keep.count(e.formID)) {
                    scoped.push_back(std::move(e));
                }
            }
            logger::info("[voice] scope-at-scale ON: {} entries -> {} (favorited/equipped spells "
                         "+ all powers/shouts)", roster.size(), scoped.size());
            roster = std::move(scoped);
        }

        // ---- Cheap change-detection (the ticker calls this every few seconds) -----
        // Hash the config + every entry (FormID, category, name) + shout unlock bits.
        // If nothing changed since last time, bail BEFORE the expensive phrase build
        // (40k-string assembly + sort) and before any logging — this is the common
        // case and was previously burning ~40ms of main-thread time every tick.
        std::uint64_t sig = 1469598103934665603ull;  // FNV-1a
        auto mix = [&sig](std::uint64_t v) {
            sig ^= v; sig *= 1099511628211ull;
        };
        auto mixStr = [&](const std::string& s) {
            for (unsigned char c : s) mix(c);
            mix(0xFF);
        };
        mix(_defaultCast ? 1 : 2);
        mix(static_cast<std::uint64_t>(_equipHand) + 10);
        for (const auto& e : roster) {
            mix(e.formID);
            mix(static_cast<std::uint64_t>(e.category) + 100);
            mixStr(e.name);
            if (e.category == Category::Shout && e.form) {
                if (auto* sh = e.form->As<RE::TESShout>()) {
                    for (int lvl = 0; lvl < RE::TESShout::VariationIDs::kTotal; ++lvl) {
                        const auto& v = sh->variations[lvl];
                        mix(v.word && v.word->GetKnown() ? (lvl + 1) : 0);
                    }
                }
            }
        }
        if (_rosterSigValid && sig == _rosterSig) {
            return;  // unchanged — no rebuild, no log spam
        }
        _rosterSigValid = true;
        _rosterSig = sig;
        logger::info("[voice] roster changed: {} entries — rebuilding grammar", roster.size());

        std::vector<CommandSpec> specs;
        specs.reserve(roster.size());
        for (const auto& e : roster) {
            specs.push_back({ e.name, e.category });
        }

        GrammarResult grammar = BuildGrammar(specs, _defaultCast, _equipHand);

        // ---- Hard phrase cap (bounded candidate set) ------------------------------
        // The phrase list seeds the fuzzy candidate set searched on every utterance, so a
        // runaway spell list (e.g. the `psb` console command adds every spell in the game
        // -> tens of thousands of phrases) would bloat memory and slow each match. Cap the
        // bulk spell/equip phrases; per-word shout + global command phrases are added
        // AFTER the cap below so the important commands always survive.
        constexpr std::size_t kMaxBulkPhrases = 6000;
        if (grammar.phrases.size() > kMaxBulkPhrases) {
            logger::warn("[voice] grammar has {} phrases — capping to {} ({} dropped). This "
                         "usually means a huge spell list (e.g. the 'psb' console command "
                         "added every spell).",
                grammar.phrases.size(), kMaxBulkPhrases,
                grammar.phrases.size() - kMaxBulkPhrases);
            grammar.phrases.resize(kMaxBulkPhrases);  // map keeps extras (harmless/unused)
        }

        // Per-word shout phrases. We accept BOTH the cumulative Dovahzul ("fus" ->
        // word 1, "fus ro" -> word 2, "fus ro dah" -> word 3) AND the cumulative
        // English translation ("force" / "force balance" / "force balance push").
        // Either fires the same word level. Only UNLOCKED levels are added, so a level
        // you haven't earned can't be spoken.
        //
        // The Dovahzul word is derived from the word-of-power EditorID (e.g. "WordDah"
        // -> "dah"), NOT the FULL name: vanilla stores a dragon-font cipher in FULL
        // (e.g. "Dah" is literally "D4"), which is unpronounceable to the offline
        // model. EditorIDs are kept in memory by po3 Tweaks (present in LoreRim).
        // Decode the dragon-font cipher used in Word-of-Power FULL names. The runes for
        // vowel digraphs are stored as DIGITS. The full rune set is 1=aa, 2=ei, 3=ii, 4=ah,
        // 5=uu, 6=ur, 7=ir, 8=oo, 9=ey (1-4/7-9 verified against the vanilla+DLC word set;
        // 5/6 from the Dragon alphabet, unused by any vanilla shout). So "D4"->"Dah",
        // "F2m"->"Feim", "T8r"->"Toor", "N7"->"Nir", "M9"->"Mey". Letters pass through; any
        // other digit leaves a digit behind and we fall back to the English translation.
        auto decodeDragonCipher = [](const std::string& full) -> std::string {
            std::string out;
            out.reserve(full.size() + 6);
            for (char c : full) {
                switch (c) {
                    case '1': out += "aa"; break;
                    case '2': out += "ei"; break;
                    case '3': out += "ii"; break;
                    case '4': out += "ah"; break;
                    case '5': out += "uu"; break;  // (Dragon-alphabet rune; unused by vanilla)
                    case '6': out += "ur"; break;  // (Dragon-alphabet rune; unused by vanilla)
                    case '7': out += "ir"; break;  // WordNir=N7, WordMir=M7
                    case '8': out += "oo"; break;
                    case '9': out += "ey"; break;  // WordMey=M9
                    default:  out.push_back(c); break;
                }
            }
            return out;
        };
        auto dovahFromWord = [&decodeDragonCipher](RE::TESWordOfPower* w) -> std::string {
            const char* eid = w->GetFormEditorID();
            std::string s = eid ? eid : "";
            // Strip a leading "Word" prefix (vanilla convention: WordFus/WordRo/...).
            if (s.size() > 4 && (s.compare(0, 4, "Word") == 0 || s.compare(0, 4, "word") == 0)) {
                s = s.substr(4);
            }
            std::string norm = NormalizePhrase(s);
            // Clean EditorID-derived romanization? use it (po3 Tweaks keeps EditorIDs).
            if (!norm.empty() && norm.find_first_of("0123456789") == std::string::npos) {
                return norm;
            }
            // EditorID empty/ciphered (po3 Tweaks off, as in this user's log): DECODE the
            // dragon-font cipher in the FULL name so "fus ro dah" et al. are generated.
            std::string decoded =
                NormalizePhrase(decodeDragonCipher(w->GetFullName() ? w->GetFullName() : ""));
            if (!decoded.empty() && decoded.find_first_of("0123456789") == std::string::npos) {
                return decoded;
            }
            return {};  // give up -> the English translation path still covers this word
        };

        for (std::size_t i = 0; i < roster.size(); ++i) {
            const auto& e = roster[i];
            if (e.category != Category::Shout || !e.form) continue;
            auto* shout = e.form->As<RE::TESShout>();
            if (!shout) continue;

            std::string cumDragon;   // raw romanization: "fus" / "fus ro" / "fus ro dah"
            std::string cumResp;     // re-spelled for g2p:  "foos" / "foos roh" / "foos roh dah"
            std::string cumTrans;    // English:          "force" / "force balance" / "force balance push"
            auto addPhrase = [&](const std::string& p, int lvl) {
                if (p.empty()) return;
                if (grammar.map.find(p) == grammar.map.end()) {
                    grammar.map[p] = CommandTarget{ i, Action::Cast, Hand::Right, false, lvl };
                    grammar.phrases.push_back(p);
                }
            };
            for (int lvl = 0; lvl < RE::TESShout::VariationIDs::kTotal; ++lvl) {
                const auto& v = shout->variations[lvl];
                if (!v.word || !v.spell) break;
                std::string dragon = dovahFromWord(v.word);
                std::string trans  = NormalizePhrase(v.word->translation.c_str());
                // Word 1 is treated as known (reverted): the shout being in the roster
                // means at least its first word is usable, and GetKnown() on word 1 can
                // read false in cases where the word is in fact castable.
                const bool known = (lvl == RE::TESShout::VariationIDs::kOne) || v.word->GetKnown();
                if (dragon.empty() && trans.empty()) break;
                std::string rawWord  = dragon.empty() ? trans : dragon;
                std::string respWord = dragon.empty() ? std::string{} : DovahRespell(dragon);
                if (respWord.empty()) respWord = rawWord;  // no improvement -> reuse raw
                // DIAGNOSTIC (only with Debug logging on): exactly what each word resolves
                // to, so we can see whether word 3 yields its Dovahzul ("dah") or fell back
                // to the translation ("push") because the EditorID/FULL were unusable.
                logger::info("[voice]   shout '{}' word{}: edid='{}' full='{}' trans='{}' -> dovah='{}' resp='{}' known={}",
                    e.name, lvl + 1,
                    v.word->GetFormEditorID() ? v.word->GetFormEditorID() : "",
                    v.word->GetFullName() ? v.word->GetFullName() : "",
                    v.word->translation.c_str(), dragon, respWord, known);
                if (!cumDragon.empty()) cumDragon += " ";
                cumDragon += rawWord;
                if (!cumResp.empty()) cumResp += " ";
                cumResp += respWord;
                if (!cumTrans.empty()) cumTrans += " ";
                cumTrans += trans.empty() ? dragon : trans;
                if (!known) break;  // higher levels locked -> stop
                // Three spoken forms, all firing this exact word level: raw Dovahzul,
                // g2p-friendly re-spelled Dovahzul, and English. addPhrase dedups.
                addPhrase(cumDragon, lvl);
                addPhrase(cumResp, lvl);
                addPhrase(cumTrans, lvl);
            }

            // Bare shout NAME ("unrelenting force") + explicit "cast/shout <name>"
            // must ALWAYS trigger THIS shout at the highest word level currently
            // known (shoutLevel -1, resolved in CastShoutNow) — the exact same
            // result as speaking its words ("fus ro dah" / "force balance push").
            //
            // Why this needs a FORCED override: a castable Spell that shares the
            // shout's display name (in many load orders a shout's word-spell or a
            // mod ability is exposed under that name) is harvested into the roster
            // BEFORE shouts, so in BuildGrammar it claims the bare-name and
            // "cast <name>" phrases first; the equal-priority Shout cannot reclaim
            // them (ties keep the earlier spec, CommandGrammar.cpp). That made
            // saying the shout's NAME hand-cast a spell with NO cooldown/animation
            // (observed: "unrelenting force" -> Cast Spell, hand=R, repeatable),
            // while the per-word phrases correctly cast the shout. Forcing the
            // shout to own its own cast-intent name phrases unifies all three entry
            // points (name / Dovahzul words / English alias) onto one CastShoutNow
            // path with one identical result. "equip <name>" is left untouched so a
            // same-named spell can still be equipped if one genuinely exists.
            auto forceShoutName = [&](const std::string& p) {
                if (p.empty()) return;
                if (grammar.map.find(p) == grammar.map.end()) {
                    grammar.phrases.push_back(p);
                }
                grammar.map[p] = CommandTarget{ i, Action::Cast, Hand::Right, false, -1 };
            };
            forceShoutName(NormalizePhrase(e.name));
            forceShoutName(NormalizePhrase("cast " + e.name));
            forceShoutName(NormalizePhrase("shout " + e.name));
        }

        {
            std::lock_guard<std::mutex> lock(_mapMutex);
            _roster = std::move(roster);
            _phraseMap = std::move(grammar.map);
        }

        // Fixed global commands (listen toggle, utility, keybind-free actions) are
        // always recognizable; dispatched via GlobalCommandPhrases() in HandleResult.
        for (const auto& kv : GlobalCommandPhrases()) {
            grammar.phrases.push_back(kv.first);
        }
        // "wait one hour" .. "wait twenty four hours" (parsed in HandleResult).
        for (int n = 1; n <= 24; ++n) {
            grammar.phrases.push_back(std::string("wait ") + NumberWords()[n] + (n == 1 ? " hour" : " hours"));
        }

        // Build the bounded fuzzy candidate set for sherpa (P2 fix).
        // We use grammar.phrases (already capped to kMaxBulkPhrases + shout + global +
        // wait phrases — a few thousand max) rather than iterating the full _phraseMap
        // which can reach 40k+ entries after a 'psb'.  The list already contains the
        // map keys (or a bounded subset thereof), the global command phrases, and the
        // wait phrases — exactly what DispatchExact searches.  Store normalized so the
        // DispatchExact lookup succeeds without an extra NormalizePhrase call.
        {
            std::vector<std::string> fuzzyCands;
            fuzzyCands.reserve(grammar.phrases.size());
            for (const auto& p : grammar.phrases) {
                fuzzyCands.push_back(NormalizePhrase(p));
            }
            std::lock_guard<std::mutex> lock(_mapMutex);
            _fuzzyCandidates = std::move(fuzzyCands);
        }

        // Skip the (off-thread) recognizer rebuild entirely if the spoken phrase
        // set is unchanged — most SpellsLearned events re-add existing/filtered
        // spells and don't change the grammar at all.
        std::vector<std::string> sorted = grammar.phrases;
        std::sort(sorted.begin(), sorted.end());
        if (sorted == _lastPhrasesSorted) {
            logger::info("[voice] grammar unchanged ({} phrases) — no rebuild", sorted.size());
            return;
        }
        _lastPhrasesSorted = std::move(sorted);

        // The recognizer is open-vocabulary — no grammar push needed. SetGrammar is a
        // no-op but call it so the phrase list is available for future hotword wiring.
        SherpaRecognizer::Get().SetGrammar(grammar.phrases);
        logger::info("[voice] grammar queued: {} phrases from {} entries ({} name conflicts resolved)",
            grammar.phrases.size(), specs.size(), grammar.collisions);
        if (!grammar.conflicts.empty()) {
            // Conflict resolution: two spells produced the same spoken phrase. We keep
            // the higher-priority/earlier one deterministically; list them so the user
            // (or a future MCM) can see which names clash and rename one in-game.
            std::string list;
            for (size_t k = 0; k < grammar.conflicts.size(); ++k) {
                if (k) list += ", ";
                list += '"' + grammar.conflicts[k] + '"';
            }
            logger::info("[voice] conflicting phrases (kept first, dropped rest): {}", list);
        }
    }

    void VoiceController::OnPhraseRecognized(const std::string& phrase)
    {
        HandleResult(phrase);
    }

    // DispatchExact — runs the exact-match lookup tables (global commands, wait
    // phrases, _phraseMap) on a phrase that is ALREADY normalized.  Never calls
    // BestFuzzyMatch; never recurses.  Used by both the direct (non-sherpa) path
    // and the sherpa fuzzy branch to ensure no re-entry into the fuzzy matcher.
    void VoiceController::DispatchExact(const std::string& phrase)
    {
        // Global commands first. The listen toggle is always honored (so you can
        // resume); everything else is ignored while paused.
        if (auto git = GlobalCommandPhrases().find(phrase); git != GlobalCommandPhrases().end()) {
            const GlobalAction action = git->second;
            if (action == GlobalAction::StartListening) {
                g_toggleOn.store(true);
                logger::info("[voice] listening ON (voice)");
                return;
            }
            if (action == GlobalAction::StopListening) {
                g_toggleOn.store(false);
                logger::info("[voice] listening OFF (voice)");
                return;
            }
            if (!EffectiveListening()) return;
            logger::info("[voice] heard '{}' -> global command", phrase);
            RunGlobalAction(action);
            return;
        }

        if (!EffectiveListening()) {
            return;  // not listening — ignore spell/shout commands
        }

        // "wait N hours" — parameterized command (not in the fixed phrase map).
        if (int hrs = ParseWaitHours(phrase); hrs > 0) {
            logger::info("[voice] heard '{}' -> wait {} hours", phrase, hrs);
            RunWaitHours(hrs);
            return;
        }

        RosterEntry entry{};
        CommandTarget target{};
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(_mapMutex);
            if (auto it = _phraseMap.find(phrase); it != _phraseMap.end()) {
                target = it->second;
                if (target.specIndex < _roster.size()) {
                    entry  = _roster[target.specIndex];
                    found  = true;
                }
            }
        }
        if (!found) {
            logger::info("[voice] heard '{}' — no command match", phrase);
            return;
        }
        logger::info("[voice] heard '{}' -> {} {} '{}' (hand={}{}{})",
            phrase, ActionName(target.action), CategoryName(entry.category), entry.name,
            HandName(target.hand), target.dual ? ", dual" : "",
            target.shoutLevel >= 0 ? std::format(", word {}", target.shoutLevel + 1) : std::string{});
        Execute(entry, target.action, target.hand, target.dual, target.shoutLevel,
                target.stanceOrigin);  // marshals to main thread
    }

    void VoiceController::HandleResult(const std::string& rawPhrase)
    {
        // Normalize the recognized text the same way map keys were built so a stray
        // case/space can't cause a silent miss.
        const std::string phrase = NormalizePhrase(rawPhrase);
        if (phrase.empty()) return;

        // Open-vocab transcripts rarely match a command phrase exactly, so check the
        // exact-match tables first, then fall back to the phonetic + edit-distance fuzzy
        // matcher against the bounded candidate set built in RefreshGrammar.
        //
        // IMPORTANT: a fuzzy match dispatches via DispatchExact (not HandleResult), so the
        // fuzzy branch is NEVER re-entered — no recursion risk even if NormalizePhrase is
        // not idempotent on the candidate or the candidate somehow mismatches.
        bool exactHit = false;
        if (GlobalCommandPhrases().count(phrase)) {
            exactHit = true;
        } else if (ParseWaitHours(phrase) > 0) {
            exactHit = true;
        } else {
            std::lock_guard<std::mutex> lock(_mapMutex);
            exactHit = (_phraseMap.count(phrase) > 0);
        }

        if (exactHit) {
            DispatchExact(phrase);
            return;
        }

        // Fuzzy fallback. _fuzzyCandidates is written on the main thread in RefreshGrammar
        // and read here on the mic thread.  The worst case is a torn read on a refresh
        // boundary: a stale or partially-updated list, which is acceptable — the next
        // utterance will use the updated list.
        std::vector<std::string> candidates;
        {
            std::lock_guard<std::mutex> lock(_mapMutex);
            candidates = _fuzzyCandidates;  // copy under lock (bounded size)
        }

        FuzzyResult fuzzy = BestFuzzyMatch(phrase, candidates);
        if (fuzzy.score >= static_cast<double>(_sherpaMatchThreshold)) {
            logger::info("[voice] transcript '{}' -> match '{}' (score {:.2f})",
                phrase, fuzzy.phrase, fuzzy.score);
            // Candidate strings are already normalized (built from normalized grammar
            // phrases) so no further NormalizePhrase is needed.
            DispatchExact(fuzzy.phrase);
        } else {
            logger::info("[voice] transcript '{}' -> no match (best '{}' {:.2f})",
                phrase, fuzzy.phrase, fuzzy.score);
        }
    }
}
