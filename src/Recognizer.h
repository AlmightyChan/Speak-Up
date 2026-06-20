#pragma once

// ============================================================================
// Recognizer — in-process Vosk speech recognition (replaces the external
// companion, which WDAC blocked as an unsigned exe).
//
// THREADING MODEL (designed for low latency + no main-thread hitches):
//   - A single background WORKER thread: stages assets, loads libvosk + model,
//     creates the recognizer, starts the mic, then services grammar rebuilds.
//   - The MIC worker thread (Win32 waveIn callback) feeds audio and dispatches
//     recognized phrases.
//   - The MAIN (game) thread only ever hands us a phrase LIST (SetGrammar);
//     it never does Vosk work, so grammar changes never stutter the game.
//
// Grammar rebuilds: the worker builds the NEW recognizer off-thread and swaps it
// in atomically under _gate, while recognition keeps running on the old grammar.
// Bursts of changes are debounced/coalesced; a failed build keeps the old one.
// ============================================================================

#include "PCH.h"
#include "VoskLoader.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace VSC
{
    class MicCapture;  // header-only; included in the .cpp

    class Recognizer
    {
    public:
        using PhraseHandler = std::function<void(const std::string&)>;

        static Recognizer& Get();

        // Begin async init (stage assets, load libvosk + model, start mic). Safe once.
        void Start(PhraseHandler a_onPhrase);

        // Request the active grammar be (re)built to these phrases. NON-BLOCKING:
        // stores the list and wakes the worker; no Vosk work on the caller's thread.
        void SetGrammar(const std::vector<std::string>& a_phrases);

        // Minimum per-result confidence (0..1) to accept a phrase. 0 = accept all
        // (default). Higher = stricter (fewer false casts, more "didn't hear that").
        void SetSensitivity(float a_required) { _sensitivity.store(a_required); }

        // End-of-utterance threshold (seconds): once the recognizer's best guess stops
        // changing for this long, finalize + dispatch immediately rather than waiting for
        // Vosk's own (slower) silence endpoint. Lower = snappier casts; too low can cut
        // off multi-word phrases. (Vosk 0.3.45 exposes no endpointer API, so we time the
        // partial-result stability ourselves.)
        void SetUtteranceThreshold(float a_seconds) { _utteranceThreshold.store(a_seconds); }

        // Force-finalize the current utterance and dispatch it (used on push-to-talk
        // release so a command spoken right before letting go still fires).
        void Finalize();

        void Stop();

        ~Recognizer();

    private:
        Recognizer() = default;
        void WorkerLoop();
        bool InitEngine();                 // stage + load model + initial recognizer + mic
        void OnAudio(const char* a_data, int a_len);

        VoskLoader      _vosk;
        VoskModel*      _model = nullptr;
        VoskRecognizer* _rec = nullptr;
        std::mutex      _gate;             // guards _rec (mic thread + worker swap)

        std::mutex                  _grammarMutex;   // guards _desiredPhrases/_dirty
        std::condition_variable     _cv;
        std::vector<std::string>    _desiredPhrases;
        bool                        _dirty = false;

        std::atomic<bool>           _started{ false };
        std::atomic<bool>           _stop{ false };
        std::atomic<float>          _sensitivity{ 0.0f };  // min confidence (0 = off)
        std::atomic<float>          _utteranceThreshold{ 1.0f };  // sec of partial stability -> finalize
        std::once_flag              _stopOnce;

        // Custom-endpoint state (mic thread only). Tracks how long the partial guess has
        // been unchanged so we can finalize early.
        std::string                       _partialText;
        std::chrono::steady_clock::time_point _partialChange{};

        PhraseHandler               _onPhrase;
        std::unique_ptr<MicCapture> _mic;
        std::thread                 _worker;
    };
}
