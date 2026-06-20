#include "PCH.h"
#include "VoiceController.h"
#include "Equipper.h"
#include "Recognizer.h"
#include "Vocabulary.h"
#include "PhraseNormalize.h"
#include "GlobalCommands.h"

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
        void Notify(const char* a_msg)
        {
            if (auto* task = SKSE::GetTaskInterface()) {
                task->AddTask([a_msg]() { RE::DebugNotification(a_msg); });
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

        // Dovahzul word -> an ENGLISH re-spelling whose grapheme-to-phoneme guess is
        // closer to how the word is actually spoken, so Vosk's offline g2p recognizes
        // it. (Vosk grammars take TEXT only — no phoneme input — so we cannot supply a
        // phoneme string like DBU's SAPI grammar did; respelling is the equivalent.)
        // Curated table covers the vanilla Words of Power; anything else falls back to
        // regular Dovahzul vowel rules, so modded shouts still get a usable variant.
        std::string DovahRespell(const std::string& w)
        {
            static const std::unordered_map<std::string, std::string> kMap = {
                {"fus","foos"},{"ro","roh"},{"dah","dah"},
                {"yol","yoll"},{"toor","tor"},{"shul","shool"},
                {"fo","foh"},{"krah","krah"},{"diin","deen"},
                {"wuld","woold"},{"nah","nah"},{"kest","kest"},
                {"feim","faym"},{"zii","zee"},{"gron","grone"},
                {"laas","lahs"},{"yah","yah"},{"nir","neer"},
                {"iiz","eez"},{"slen","slen"},{"nus","noos"},
                {"tiid","teed"},{"klo","kloh"},{"ul","ool"},
                {"lok","loke"},{"vah","vah"},{"koor","koor"},
                {"strun","stroon"},{"bah","bah"},{"qo","koh"},
                {"su","soo"},{"grah","grah"},{"dun","doon"},
                {"zun","zoon"},{"haal","hahl"},{"viik","veek"},
                {"faas","fahs"},{"ru","roo"},{"maar","mar"},
                {"joor","yor"},{"zah","zah"},{"frul","frool"},
                {"kaan","kahn"},{"drem","drem"},{"ov","ohv"},
                {"krii","kree"},{"lun","loon"},{"aus","ows"},
                {"zul","zool"},{"mey","may"},{"gut","goot"},
                {"raan","rahn"},{"mir","meer"},{"tah","tah"},
                {"od","ohd"},{"ah","ah"},{"viing","veeng"},
                {"gol","gohl"},{"hah","hah"},{"dov","dohv"},
                {"mid","mid"},{"vur","voor"},{"shaan","shahn"},
                {"hun","hoon"},{"kaal","kahl"},{"zoor","zor"},
                {"ven","ven"},{"gaar","gar"},{"nos","nohs"},
                {"rii","ree"},{"vaaz","vahz"},{"zol","zole"},
            };
            auto it = kMap.find(w);
            if (it != kMap.end()) return it->second;

            // Fallback: regular Dovahzul vowel rules (digraphs before singles).
            std::string s = w;
            auto repl = [&](const char* from, const char* to) {
                std::string f = from, t = to; std::size_t p = 0;
                while ((p = s.find(f, p)) != std::string::npos) { s.replace(p, f.size(), t); p += t.size(); }
            };
            repl("ii", "ee"); repl("aa", "ah"); repl("uu", "oo");
            repl("ei", "ay"); repl("ey", "ay"); repl("u", "oo");
            return s == w ? std::string{} : s;  // empty = no improvement over the raw form
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
        _defaultCast = ReadCfgBool("bDefaultActionCast", "DefaultActionCast", true);

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

        Recognizer::Get().SetSensitivity(
            std::strtof(ReadCfg("fSensitivity", "Sensitivity", "0.0").c_str(), nullptr));
        Recognizer::Get().SetUtteranceThreshold(
            std::strtof(ReadCfg("fUtteranceThreshold", "UtteranceThreshold", "1.0").c_str(), nullptr));

        // Scale handling: by default include ALL known spells (the hard cap below is
        // the crash failsafe). When this toggle is ON and the list is huge, scope the
        // grammar to favorited + equipped magic (+ all powers/shouts) — see RefreshGrammar.
        _scopeAtScale = ReadCfgBool("bScopeAtScale", "ScopeAtScale", false);

        // ---- Developer options (default OFF so shipped users store no extra data) ----
        // DebugLogging: when off, the log level is raised to WARN so the high-volume
        // info lines (every recognized phrase, roster/grammar diagnostics) are NOT
        // written. Turn it ON in the MCM only while testing.
        _debugLog    = ReadCfgBool("bDebugLogging", "DebugLogging", false);
        _dumpGrammar = ReadCfgBool("bDumpGrammar", "DumpGrammar", false);
        spdlog::set_level((_debugLog || _dumpGrammar) ? spdlog::level::info : spdlog::level::warn);
        // Dump the live grammar once on the rising edge of the toggle (so a dev can
        // inspect exactly what's recognized without spamming it every refresh tick).
        if (_dumpGrammar && !_dumpGrammarPrev) {
            DumpGrammar();
        }
        _dumpGrammarPrev = _dumpGrammar;

        CastSettings cs;
        cs.instantCast        = ReadCfgBool("bInstantCast", "InstantCast", true);
        cs.allowConcentration = ReadCfgBool("bAllowConcentration", "AllowConcentration", false);
        cs.allowLongCast      = ReadCfgBool("bAllowLongCast", "AllowLongCast", false);
        cs.playShoutAnimation = ReadCfgBool("bPlayShoutAnimation", "PlayShoutAnimation", false);
        cs.longCastThreshold  = std::strtof(thr.c_str(), nullptr);
        cs.equipHand          = _equipHand;
        SetCastSettings(cs);

        // Log only on first load / when something changed (avoids 3s ticker spam).
        std::string snapshot = std::to_string(_defaultCast) + "|" + HandName(_equipHand) + "|" +
            std::to_string(cs.instantCast) + "|" + std::to_string(cs.allowConcentration) + "|" +
            std::to_string(cs.allowLongCast) + "|" + std::to_string(cs.longCastThreshold) + "|" +
            std::to_string(g_toggleKey.load()) + "+" + std::to_string(g_toggleMod.load()) + "|" +
            std::to_string(g_pttKey.load()) + "+" + std::to_string(g_pttMod.load()) + "|" +
            std::to_string(_scopeAtScale) + "|" + std::to_string(cs.playShoutAnimation);
        if (snapshot != _lastConfigSnapshot) {
            _lastConfigSnapshot = snapshot;
            logger::info("[voice] config: defaultCast={} equipHand={} instantCast={} "
                         "allowConcentration={} allowLongCast={} longThreshold={:.2f} "
                         "toggle=0x{:X}+0x{:X} ptt=0x{:X}+0x{:X} scopeAtScale={}",
                _defaultCast, HandName(_equipHand), cs.instantCast, cs.allowConcentration,
                cs.allowLongCast, cs.longCastThreshold, g_toggleKey.load(), g_toggleMod.load(),
                g_pttKey.load(), g_pttMod.load(), _scopeAtScale);
        }
    }

    void VoiceController::Start()
    {
        if (_started) return;
        _started = true;
        // In-process recognizer (no external exe — WDAC blocks unsigned exes).
        Recognizer::Get().Start([](const std::string& phrase) {
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
        Recognizer::Get().Finalize();
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
                    task->AddTask([]() { VoiceController::Get().RefreshGrammar(); });
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

        // ---- Hard grammar cap (libvosk safety) ------------------------------------
        // libvosk builds an in-memory FST from the grammar; tens of thousands of
        // phrases blow up to multiple GB and CRASH inside libvosk (observed: `psb`
        // adds ~5000 spells -> 44k phrases -> 12GB -> crash on save/load). Cap the
        // bulk spell/equip phrases; per-word shout + global command phrases are added
        // AFTER the cap below so the important commands always survive.
        constexpr std::size_t kMaxBulkPhrases = 6000;
        if (grammar.phrases.size() > kMaxBulkPhrases) {
            logger::warn("[voice] grammar has {} phrases — capping to {} to avoid a libvosk "
                         "memory crash ({} dropped). This usually means a huge spell list "
                         "(e.g. the 'psb' console command added every spell).",
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
        auto dovahFromWord = [](RE::TESWordOfPower* w) -> std::string {
            const char* eid = w->GetFormEditorID();
            std::string s = eid ? eid : "";
            // Strip a leading "Word" prefix (vanilla convention: WordFus/WordRo/...).
            if (s.size() > 4 && (s.compare(0, 4, "Word") == 0 || s.compare(0, 4, "word") == 0)) {
                s = s.substr(4);
            }
            std::string norm = NormalizePhrase(s);
            // Reject ciphered/garbage tokens (digits) — fall back to a digit-free FULL.
            if (norm.empty() || norm.find_first_of("0123456789") != std::string::npos) {
                std::string full = NormalizePhrase(w->GetFullName() ? w->GetFullName() : "");
                norm = (!full.empty() && full.find_first_of("0123456789") == std::string::npos)
                           ? full : std::string{};
            }
            return norm;
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

        Recognizer::Get().SetGrammar(grammar.phrases);
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

    void VoiceController::HandleResult(const std::string& rawPhrase)
    {
        // Normalize the recognized text the same way map keys were built so a stray
        // case/space can't cause a silent miss.
        const std::string phrase = NormalizePhrase(rawPhrase);
        if (phrase.empty()) return;

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
            if (!EffectiveListening()) return;  // not listening
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
                    entry = _roster[target.specIndex];
                    found = true;
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
}
