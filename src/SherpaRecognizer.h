#pragma once

// ============================================================================
// SherpaRecognizer — in-process sherpa-onnx streaming Zipformer speech
// recognition: open-vocabulary ASR fed live from the microphone.
//
// THREADING MODEL:
//   - WORKER thread: stages assets, loads DLLs + model, creates the
//     recognizer + one persistent stream, starts the mic, then runs the
//     mic-starvation watchdog loop.
//   - waveIn MM CALLBACK: copies PCM into the audio queue. No sherpa calls.
//   - CONSUMER thread: drains the audio queue, converts int16 -> float,
//     calls AcceptWaveform / IsReady / Decode / IsEndpoint / GetResult.
//     Consecutive decode faults beyond kFaultThreshold trigger a self-heal
//     (destroy + recreate the recognizer and stream from the stored config).
//   - MAIN (game) thread: only calls SetGrammar (no-op for sherpa) / Start.
//
// Sherpa is open-vocabulary (no grammar needed) so SetGrammar only stores the
// list for future hot-word wiring.  The recognizer produces raw text; we
// lower-case and trim it and call _onPhrase.
//
// Endpointing is delegated to sherpa's built-in endpoint detector
// (rule1 = 0.7 s trailing silence, rule2 = 0.45 s, rule3 = 20 s utterance).
//
// Self-heal: consecutive decode faults past kFaultThreshold destroy + recreate the
// recognizer+stream from the stored SherpaOnnxOnlineRecognizerConfig.
//
// Watchdog: WorkerLoop runs a kWatchdogSecs wait_for loop after InitEngine() so mic
// starvation is logged even after the first init.
//
// Decode loop cap: the inner IsReady/Decode loop is bounded to kMaxDecodeIter
// iterations per chunk; if exceeded a warning is logged and the iteration is
// treated as a fault to prevent the consumer from spinning indefinitely.
// ============================================================================

#include "PCH.h"
#include "SherpaLoader.h"
#include <atomic>
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

    class SherpaRecognizer
    {
    public:
        using PhraseHandler = std::function<void(const std::string&)>;

        static SherpaRecognizer& Get();

        // Begin async init (stage assets, load sherpa, start mic). Safe once.
        void Start(PhraseHandler a_onPhrase);

        // No-op for sherpa (open-vocab); stores phrases for future hotword wiring.
        void SetGrammar(const std::vector<std::string>& a_phrases);

        // Minimum sensitivity (0..1); stored but currently unused (sherpa has no
        // per-result confidence in the transducer C API result struct).
        void SetSensitivity(float a_required) { _sensitivity.store(a_required); }

        // Endpoint trailing-silence rules (seconds) — the recognition "responsiveness"
        // ("sensitivity") knobs. r1 = hard silence cutoff, r2 = end-of-phrase pause
        // (primary), r3 = max utterance length. Stored now; applied at the NEXT
        // InitEngine() (sherpa caches endpoint config when the recognizer is created), so
        // a live change takes effect on the next recognizer (re)start.
        void SetEndpointRules(float a_r1, float a_r2, float a_r3)
        {
            _endpointRule1.store(a_r1);
            _endpointRule2.store(a_r2);
            _endpointRule3.store(a_r3);
        }

        // Hotword (contextual-biasing) list: newline-separated phrases the recognizer should
        // lean toward (the live spell/shout roster). a_score is the per-token boost. Applied
        // at the next InitEngine(), and ONLY if a bpe.vocab sits beside the model (needed to
        // tokenize the phrases) — otherwise hotwords are skipped and recognition is unchanged.
        void SetHotwords(const std::string& a_phrases, float a_score, bool a_enabled);

        // Force-finalize (no-op here — sherpa's built-in endpoint handles this).
        void Finalize() {}

        void Stop();

        // Fully tear down and re-initialise: recovers from a stalled/dead mic AND from a
        // failed first InitEngine (mic absent at launch). Safe to call on the main thread;
        // joins worker + consumer before restarting.  Re-entrant calls are no-ops while a
        // restart is already in flight.
        void Restart();

        // True when the microphone could not be brought up after all startup attempts
        // (device absent, waveInOpen failed, or opened but delivered no audio). Polled by
        // VoiceController to surface a graceful "mic couldn't connect" message. Cleared
        // when the mic next comes up.
        bool MicStartFailed() const { return _micFailed.load(); }

        ~SherpaRecognizer();

    private:
        SherpaRecognizer() = default;

        void WorkerLoop();
        bool InitEngine();

        // Bring the mic up with immediate retries: succeeds only when the device opens AND
        // delivers real audio within kStartupAudioWaitSecs. Sets _micFailed on giving up.
        bool AcquireMic();
        // Block up to a_secs for the first real audio buffer (or _stop). Returns true if
        // audio arrived.
        bool WaitForFirstAudio(int a_secs);
        // Close + release _mic with a bounded wait (a stalled driver can hang waveInClose);
        // on overrun the handle is abandoned (session-scoped leak is safe).
        void CloseMicBounded();

        // Called by the consumer when consecutive faults exceed kFaultThreshold:
        // destroys the recognizer+stream and recreates them from _cfg.
        // Must be called WITHOUT holding _gate.
        void RebuildRecognizer();

        void EnqueueAudio(const char* a_data, int a_len);
        void ConsumerLoop();

        // PCM chunk (int16 bytes copied from waveIn buffer).
        struct AudioChunk { std::vector<char> data; };
        static constexpr int kAudioQueueMax  = 64;
        static constexpr int kWatchdogSecs   = 5;   // warn if no audio for this long
        static constexpr int kFaultThreshold = 5;   // consecutive faults before self-heal
        static constexpr int kMaxDecodeIter  = 512; // per-chunk IsReady/Decode cap
        // Upper bound on how long Stop() waits for the mic (waveIn) to close before
        // abandoning it. A stalled/disconnected audio driver — common with virtualized
        // audio in a VM — can make waveInReset/waveInClose block indefinitely; never let
        // that freeze the caller. On overrun we leak the handle (session-scoped, safe).
        static constexpr int kMicCloseTimeoutMs = 1500;
        // Startup mic acquisition: number of immediate attempts before surfacing a
        // graceful failure, how long each attempt waits for the first real audio, and the
        // pause between attempts.
        static constexpr int kMicStartAttempts     = 3;
        static constexpr int kStartupAudioWaitSecs = 2;
        static constexpr int kMicRetryBackoffMs    = 250;

        SherpaLoader                      _sherpa;
        const SherpaOnnxOnlineRecognizer* _rec    = nullptr;
        const SherpaOnnxOnlineStream*     _stream = nullptr;
        std::mutex                        _gate;   // guards _rec + _stream

        // Cached recognizer config for self-heal rebuilds.
        // String members hold backing storage for the C string pointers in the config.
        struct CfgStorage {
            std::string enc, dec, joi, tok;
            std::string bpeVocab;   // path to bpe.vocab (for hotword tokenization)
            std::string hotwords;   // newline-joined hotword phrases (backing for hotwords_buf)
        };
        CfgStorage                        _cfgStorage;
        SherpaOnnxOnlineRecognizerConfig  _cfg{};

        std::mutex              _audioMutex;
        std::condition_variable _audioCv;
        std::deque<AudioChunk>  _audioQueue;
        bool                    _audioStop = false;

        std::atomic<bool>    _started{ false };
        std::atomic<bool>    _stop{ false };
        std::atomic<float>   _sensitivity{ 0.0f };
        // Endpoint trailing-silence rules (seconds), applied to _cfg at InitEngine().
        // Defaults mirror the values that were previously hardcoded there.
        std::atomic<float>   _endpointRule1{ 0.7f };   // hard silence cutoff
        std::atomic<float>   _endpointRule2{ 0.45f };  // end-of-phrase pause (primary)
        std::atomic<float>   _endpointRule3{ 20.0f };  // max utterance length
        // Hotword (contextual-biasing) state. _hotwordsText is the newline-joined roster;
        // guarded by _hotwordsMutex (set on the main thread, read by InitEngine on the
        // worker). Applied at InitEngine only if enabled AND bpe.vocab is present.
        std::mutex           _hotwordsMutex;
        std::string          _hotwordsText;
        std::atomic<float>   _hotwordsScore{ 1.5f };
        std::atomic<bool>    _hotwordsEnabled{ false };
        // Replaces std::once_flag: a plain mutex+bool that is re-armable so Restart()
        // can issue a second Stop() after resetting the flag.
        std::mutex           _stopMutex;
        bool                 _stopInFlight = false;
        // Guards against concurrent Restart() calls.
        std::atomic<bool>    _restarting{ false };
        // True when the mic failed to come up after all startup attempts (polled by
        // VoiceController for a graceful notification). Cleared when the mic next succeeds.
        std::atomic<bool>    _micFailed{ false };

        // Consumer-private (consumer thread only, no lock needed).
        int _consecutiveFaults = 0;

        PhraseHandler               _onPhrase;
        std::unique_ptr<MicCapture> _mic;
        std::thread                 _worker;
        std::thread                 _consumer;
    };
}
