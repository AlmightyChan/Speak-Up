#pragma once

// ============================================================================
// Recognizer — in-process Vosk speech recognition (replaces the external
// companion, which WDAC blocked as an unsigned exe).
//
// THREADING MODEL:
//   - WORKER thread: stages assets, loads libvosk + model, creates the
//     recognizer, starts the mic, then services grammar rebuilds.
//   - waveIn MM CALLBACK: copies PCM into the audio queue. No Vosk calls here.
//   - CONSUMER thread: drains the audio queue and runs accept_waveform /
//     endpointing / dispatch.  This is the thread that may touch libvosk.
//   - MAIN (game) thread: only calls SetGrammar (non-blocking).
//
// Grammar rebuilds: the worker builds the NEW recognizer off-thread and swaps it
// in atomically under _gate, while recognition keeps running on the old grammar.
// Bursts of changes are debounced/coalesced; a failed build keeps the old one.
//
// Self-heal: the consumer counts consecutive decode faults; once the threshold
// is exceeded it rebuilds the recognizer from the current grammar so a transient
// libvosk fault recovers without going permanently silent.
//
// Watchdog: if no audio arrives for ~5 s while listening, the consumer logs a
// warning so the starvation is visible in the log.
// ============================================================================

#include "PCH.h"
#include "VoskLoader.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

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

        // When enabled, Vosk's own log level is set to 0 (instead of -1) so libvosk
        // prints which grammar words it rejects as OOV. Should only be set while debug
        // logging is active (it produces noisy output). Must be called before Start().
        void SetOovDiag(bool a_enable) { _oovDiag.store(a_enable); }

        void Stop();

        // Fully tear down and re-initialise: recovers from a stalled/dead mic AND from a
        // failed first InitEngine (mic absent at launch). Safe to call on the main thread;
        // joins worker + consumer before restarting.  Re-entrant calls are no-ops while a
        // restart is already in flight.  The grammar and sensitivity settings are preserved
        // because they live in atomics/_desiredPhrases which are not cleared on Stop.
        void Restart();

        ~Recognizer();

    private:
        Recognizer() = default;

        void WorkerLoop();
        bool InitEngine();                 // stage + load model + initial recognizer + mic

        // Called from the waveIn MM callback: copies PCM bytes into _audioQueue.
        // Must be fast and must NOT call any Vosk API.
        void EnqueueAudio(const char* a_data, int a_len);

        // Dedicated thread: drains _audioQueue, runs Vosk decode + endpointing.
        void ConsumerLoop();

        // Rebuild the recognizer from _desiredPhrases (current grammar).
        // Called by the consumer after too many consecutive faults.
        // Must be called WITHOUT holding _gate.
        void RebuildRecognizer();

        VoskLoader      _vosk;
        VoskModel*      _model = nullptr;
        VoskRecognizer* _rec = nullptr;
        std::mutex      _gate;             // guards _rec (consumer thread + worker swap)

        // ---- Grammar rebuild (worker thread) ------------------------------------
        std::mutex                  _grammarMutex;   // guards _desiredPhrases/_dirty
        std::condition_variable     _cv;
        std::vector<std::string>    _desiredPhrases;
        bool                        _dirty = false;

        // ---- Audio queue (waveIn MM -> consumer thread) -------------------------
        // Each entry is a PCM chunk copied out of the waveIn buffer.
        // Capped at kAudioQueueMax: if the consumer falls behind, the oldest entry
        // is silently dropped so the queue can never grow unbounded.
        struct AudioChunk { std::vector<char> data; };
        static constexpr int kAudioQueueMax = 64;   // ~6.4 s of 100 ms buffers
        static constexpr int kWatchdogSecs  = 5;    // warn if no audio for this long
        static constexpr int kFaultThreshold = 5;   // consecutive faults before self-heal

        std::mutex              _audioMutex;
        std::condition_variable _audioCv;
        std::deque<AudioChunk>  _audioQueue;
        bool                    _audioStop = false;  // tells ConsumerLoop to exit

        // ---- Lifetime -----------------------------------------------------------
        std::atomic<bool>           _started{ false };
        std::atomic<bool>           _stop{ false };
        std::atomic<bool>           _oovDiag{ false };
        std::atomic<float>          _sensitivity{ 0.0f };
        std::atomic<float>          _utteranceThreshold{ 1.0f };
        // Replaces std::once_flag: a plain mutex+bool that is re-armable so Restart()
        // can issue a second Stop() after resetting the flag.
        std::mutex                  _stopMutex;
        bool                        _stopInFlight = false;
        // Guards against concurrent Restart() calls (e.g. two menu-close events racing).
        std::atomic<bool>           _restarting{ false };

        // ---- Consumer-private state (consumer thread only, no lock needed) ------
        std::string                            _partialText;
        std::chrono::steady_clock::time_point  _partialChange{};
        int                                    _consecutiveFaults = 0;

        // Set by Finalize() to tell the consumer to discard its partial state so
        // the same utterance cannot be dispatched twice (Finalize already sent it).
        // Written by Finalize() (main thread), read+cleared by consumer thread under
        // _gate — using an atomic avoids a separate mutex.
        std::atomic<bool>                      _resetEndpoint{ false };

        PhraseHandler               _onPhrase;
        std::unique_ptr<MicCapture> _mic;
        std::thread                 _worker;
        std::thread                 _consumer;
    };
}
