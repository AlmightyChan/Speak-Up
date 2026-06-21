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

        // Force-finalize (no-op here — sherpa's built-in endpoint handles this).
        void Finalize() {}

        void Stop();

        // Fully tear down and re-initialise: recovers from a stalled/dead mic AND from a
        // failed first InitEngine (mic absent at launch). Safe to call on the main thread;
        // joins worker + consumer before restarting.  Re-entrant calls are no-ops while a
        // restart is already in flight.
        void Restart();

        ~SherpaRecognizer();

    private:
        SherpaRecognizer() = default;

        void WorkerLoop();
        bool InitEngine();

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

        SherpaLoader                      _sherpa;
        const SherpaOnnxOnlineRecognizer* _rec    = nullptr;
        const SherpaOnnxOnlineStream*     _stream = nullptr;
        std::mutex                        _gate;   // guards _rec + _stream

        // Cached recognizer config for self-heal rebuilds.
        // String members hold backing storage for the C string pointers in the config.
        struct CfgStorage {
            std::string enc, dec, joi, tok;
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
        // Replaces std::once_flag: a plain mutex+bool that is re-armable so Restart()
        // can issue a second Stop() after resetting the flag.
        std::mutex           _stopMutex;
        bool                 _stopInFlight = false;
        // Guards against concurrent Restart() calls.
        std::atomic<bool>    _restarting{ false };

        // Consumer-private (consumer thread only, no lock needed).
        int _consecutiveFaults = 0;

        PhraseHandler               _onPhrase;
        std::unique_ptr<MicCapture> _mic;
        std::thread                 _worker;
        std::thread                 _consumer;
    };
}
