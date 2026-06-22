#pragma once

// ============================================================================
// VoiceController — the glue. Builds the live roster, derives the command grammar
// (aliases x verbs via CommandGrammar), pushes the phrase list to the IN-PROCESS
// recognizer, and on a recognized phrase maps it back to (roster entry, action,
// hand, dual) and executes it. Owns the roster + phrase map (written on the main
// thread when the roster changes; read on the recognizer/mic thread on a result).
// ============================================================================

#include "PCH.h"
#include "Types.h"
#include "SpellRoster.h"
#include "CommandGrammar.h"
#include <atomic>
#include <mutex>
#include <thread>

namespace VSC
{
    class VoiceController
    {
    public:
        static VoiceController& Get();

        void Start();           // kPostLoad: start the in-process recognizer
        void MarkGameReady();   // kPostLoadGame/kNewGame: enable + refresh (main thread)
        // Rebuild + push the grammar (main thread). a_force=true (events: spells learned,
        // transforms, save load) always rebuilds; a_force=false (the poll) only rebuilds
        // when a shout's word-unlock state changed — spells/transforms are event-driven.
        void RefreshGrammar(bool a_force = false);
        void RegisterEvents();  // SpellsLearned + transform sinks (main thread)

        // Called by the recognizer on the mic thread with a recognized phrase.
        void OnPhraseRecognized(const std::string& phrase);

        // Dump the LIVE grammar (phrase -> command) to the log (dev key).
        void DumpGrammar();

        void ToggleListening();        // persistent listen toggle (hotkey or voice)
        void OnPushToTalkReleased();   // flush tail utterance when PTT key is released

        // Restart the recognizer without relaunching the game.  Notifies the player and
        // re-initialises the recognizer + mic.  Must run on the main thread (called via
        // AddTask / menu-close path from LoadConfig).
        void RestartRecognizer();

    private:
        VoiceController() = default;

        void HandleResult(const std::string& rawPhrase);
        // Dispatch a phrase that is already normalized and known to exist in the
        // exact-match tables (phraseMap / global commands / wait phrases).
        // Called by HandleResult directly AND by the fuzzy branch to avoid recursion.
        // a_allowFuzzy is always false here; the parameter is kept as a firewall so
        // adding a second fuzzy level in the future would require an explicit opt-in.
        void DispatchExact(const std::string& normalizedPhrase);
        void LoadConfig();
        // Read the sherpa endpoint trailing-silence rules ("sensitivity"/responsiveness)
        // from MCM/INI and push them to the recognizer. They take effect at the next
        // recognizer (re)start (sherpa caches endpoint config at creation).
        void PushEndpointRules();
        // Build the hotword (contextual-biasing) list from the live roster and push it to the
        // recognizer. Takes effect on the next recognizer (re)start (sherpa bakes hotwords at
        // creation), same as the endpoint rules. No-op cost when hotwords are disabled.
        void PushHotwords();
        // Edge-triggered: surface a graceful in-game notice when the recognizer reports the
        // mic couldn't start (and a "connected" notice when it recovers). Called from the
        // ticker so it runs only while in-game.
        void CheckMicStatus();
        void StartTicker();   // periodic refresh to catch transforms / word unlocks

        std::mutex                                     _mapMutex;
        std::vector<RosterEntry>                       _roster;
        std::unordered_map<std::string, CommandTarget> _phraseMap;
        std::vector<std::string>                       _lastPhrasesSorted;  // skip-if-unchanged
        std::vector<std::string>                       _fuzzyCandidates;    // bounded candidate set for sherpa fuzzy match
        std::uint64_t                                  _rosterSig = 0;      // cheap change-detect (roster+config)
        bool                                           _rosterSigValid = false;
        std::uint64_t                                  _shoutSig = 0;       // poll: shout word-unlock state
        bool                                           _shoutSigValid = false;
        bool                                           _defaultCast = true;
        bool                                           _scopeAtScale = false;  // favorites-scope huge lists
        bool                                           _debugLog = false;      // dev: info-level logging
        bool                                           _dumpGrammar = false;   // dev: dump live grammar on enable
        bool                                           _dumpGrammarPrev = false;
        Hand                                           _equipHand = Hand::Left;
        bool                                           _listenDefaultsApplied = false;  // base listen state initialized
        bool                                           _pttBoundPrev = false;           // re-eval base when PTT bound-state changes
        std::string                                    _lastConfigSnapshot;  // log-on-change
        bool                                           _started = false;
        std::atomic<bool>                              _gameReady{ false };
        std::thread                                    _ticker;
        std::atomic<bool>                              _tickerStarted{ false };
        float                                          _sherpaMatchThreshold = 0.62f;  // fuzzy match min score
        bool                                           _restartReqPrev = false;        // edge-trigger: bRestartRecognizer
        bool                                           _micFailNotified = false;       // edge-trigger: mic-couldn't-start notice
    };
}
