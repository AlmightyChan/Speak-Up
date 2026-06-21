#include "PCH.h"
#include "SherpaRecognizer.h"
#include "MicCapture.h"

#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace logger = SKSE::log;

namespace VSC
{
    namespace
    {
        // ---- Path helpers (identical to those in Recognizer.cpp) ----------------

        std::filesystem::path PluginDir()
        {
            // P2 fix: check GetModuleHandleExW success; use a grow-on-
            // ERROR_INSUFFICIENT_BUFFER loop so long install paths (e.g. deep MO2
            // profile trees) do not silently truncate at MAX_PATH.
            HMODULE self = nullptr;
            if (!::GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(&PluginDir), &self)) {
                logger::error("[sherpa] GetModuleHandleExW failed ({})", ::GetLastError());
                return {};
            }
            std::wstring buf(MAX_PATH, L'\0');
            for (;;) {
                DWORD n = ::GetModuleFileNameW(self, buf.data(), static_cast<DWORD>(buf.size()));
                if (n == 0) {
                    logger::error("[sherpa] GetModuleFileNameW failed ({})", ::GetLastError());
                    return {};
                }
                if (n < static_cast<DWORD>(buf.size())) {
                    buf.resize(n);  // trim to actual length
                    break;
                }
                // Buffer was too small (return == size means truncated on Win32).
                if (buf.size() >= 32768) {
                    logger::error("[sherpa] GetModuleFileNameW: path exceeds 32768 chars — giving up");
                    return {};
                }
                buf.resize(buf.size() * 2);
            }
            return std::filesystem::path(buf).parent_path();
        }

        std::filesystem::path RealDestDir()
        {
            wchar_t buf[MAX_PATH]{};
            DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
            if (n == 0 || n >= MAX_PATH) return {};
            return std::filesystem::path(buf) / "SpeakUp";
        }

        // Remove anything under dest that src no longer contains, so the staged copy
        // is an exact MIRROR of the installed build (prevents a stale model/runtime from
        // a prior build/update from lingering and being picked up, and bounds disk use).
        void MirrorPrune(const std::filesystem::path& src, const std::filesystem::path& dest)
        {
            std::error_code ec;
            if (!std::filesystem::exists(dest, ec)) return;
            std::vector<std::filesystem::path> orphans;
            std::error_code itEc;
            for (std::filesystem::recursive_directory_iterator it(dest, itEc), end;
                 !itEc && it != end; it.increment(itEc)) {
                std::error_code rEc;
                const auto rel = std::filesystem::relative(it->path(), dest, rEc);
                if (rEc || rel.empty()) { itEc.clear(); continue; }
                std::error_code sEc;
                if (!std::filesystem::exists(src / rel, sEc)) {
                    orphans.push_back(it->path());
                    it.disable_recursion_pending();
                }
            }
            for (const auto& o : orphans) {
                std::error_code rmEc;
                std::filesystem::remove_all(o, rmEc);
            }
            if (!orphans.empty()) {
                logger::info("[sherpa] staging mirror: pruned {} stale path(s) (old model/runtime)",
                    orphans.size());
            }
        }

        bool SyncTree(const std::filesystem::path& src, const std::filesystem::path& dest)
        {
            std::error_code ec;
            std::filesystem::create_directories(dest, ec);
            MirrorPrune(src, dest);  // make dest exactly mirror the installed build
            std::filesystem::copy(src, dest,
                std::filesystem::copy_options::recursive |
                std::filesystem::copy_options::update_existing, ec);
            if (ec) {
                logger::error("[sherpa] asset sync {} -> {} failed: {}",
                    src.string(), dest.string(), ec.message());
                return false;
            }
            return true;
        }

        // Scan %LOCALAPPDATA%\SpeakUp\models for a "sherpa-onnx-*" subfolder.
        std::filesystem::path FindSherpaModel(const std::filesystem::path& destRoot)
        {
            const auto modelsDir = destRoot / "models";
            std::error_code ec;
            if (!std::filesystem::is_directory(modelsDir, ec)) return {};
            for (const auto& entry : std::filesystem::directory_iterator(modelsDir, ec)) {
                if (ec) break;
                if (!entry.is_directory()) continue;
                const std::string name = entry.path().filename().string();
                if (name.rfind("sherpa-onnx-", 0) == 0) {
                    return entry.path();
                }
            }
            return {};
        }

        // ---- SEH-guarded sherpa call wrappers ----------------------------------
        // __try/__except cannot share a stack frame with C++ object unwinding, so
        // each wrapper is an isolated free function.

        const SherpaOnnxOnlineRecognizer* SafeCreateRecognizer(
            SherpaLoader::fn_CreateOnlineRecognizer fn,
            const SherpaOnnxOnlineRecognizerConfig* cfg)
        {
            __try {
                return fn(cfg);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return nullptr;
            }
        }

        const SherpaOnnxOnlineStream* SafeCreateStream(
            SherpaLoader::fn_CreateOnlineStream fn,
            const SherpaOnnxOnlineRecognizer* rec)
        {
            __try {
                return fn(rec);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return nullptr;
            }
        }

        // Returns: 0 = ok, -1 = SEH fault.
        int SafeAcceptWaveform(SherpaLoader::fn_AcceptWaveform fn,
                               const SherpaOnnxOnlineStream* stream,
                               int32_t sr, const float* samples, int32_t n)
        {
            __try {
                fn(stream, sr, samples, n);
                return 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -1;
            }
        }

        // Returns: 1 = ready, 0 = not ready, -1 = SEH fault.
        int SafeIsReady(SherpaLoader::fn_IsOnlineStreamReady fn,
                        const SherpaOnnxOnlineRecognizer* rec,
                        const SherpaOnnxOnlineStream* stream)
        {
            __try {
                return fn(rec, stream);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -1;
            }
        }

        // Returns: 0 = ok, -1 = SEH fault.
        int SafeDecode(SherpaLoader::fn_DecodeOnlineStream fn,
                       const SherpaOnnxOnlineRecognizer* rec,
                       const SherpaOnnxOnlineStream* stream)
        {
            __try {
                fn(rec, stream);
                return 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -1;
            }
        }

        // Returns: 1 = endpoint, 0 = not, -1 = SEH fault.
        int SafeIsEndpoint(SherpaLoader::fn_OnlineStreamIsEndpoint fn,
                           const SherpaOnnxOnlineRecognizer* rec,
                           const SherpaOnnxOnlineStream* stream)
        {
            __try {
                return fn(rec, stream);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -1;
            }
        }

        const SherpaOnnxOnlineRecognizerResult* SafeGetResult(
            SherpaLoader::fn_GetOnlineStreamResult fn,
            const SherpaOnnxOnlineRecognizer* rec,
            const SherpaOnnxOnlineStream* stream)
        {
            __try {
                return fn(rec, stream);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return nullptr;
            }
        }

        // Returns: 0 = ok, -1 = SEH fault.
        int SafeReset(SherpaLoader::fn_OnlineStreamReset fn,
                      const SherpaOnnxOnlineRecognizer* rec,
                      const SherpaOnnxOnlineStream* stream)
        {
            __try {
                fn(rec, stream);
                return 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -1;
            }
        }

        // Trim leading/trailing whitespace and lower-case the text.
        std::string NormalizeText(const char* raw)
        {
            if (!raw) return "";
            std::string s = raw;
            // lower-case
            std::transform(s.begin(), s.end(), s.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            // trim leading
            const auto a = s.find_first_not_of(" \t\r\n");
            if (a == std::string::npos) return "";
            // trim trailing
            const auto b = s.find_last_not_of(" \t\r\n");
            return s.substr(a, b - a + 1);
        }
    }

    // =========================================================================
    // Singleton
    // =========================================================================

    SherpaRecognizer& SherpaRecognizer::Get()
    {
        static SherpaRecognizer singleton;
        return singleton;
    }

    SherpaRecognizer::~SherpaRecognizer() { Stop(); }

    // =========================================================================
    // Public API
    // =========================================================================

    void SherpaRecognizer::Start(PhraseHandler a_onPhrase)
    {
        if (_started.exchange(true)) return;
        _onPhrase = std::move(a_onPhrase);
        _worker = std::thread([this] { WorkerLoop(); });
    }

    void SherpaRecognizer::SetGrammar(const std::vector<std::string>& /*a_phrases*/)
    {
        // Sherpa is open-vocabulary — no grammar needed.
        // Phrases stored here could wire hot-word boosting in a future update.
    }

    void SherpaRecognizer::Stop()
    {
        if (!_started.load()) return;

        // Idempotent + re-armable teardown (replaces std::call_once so Restart() can
        // re-arm after a previous Stop()).
        {
            std::lock_guard<std::mutex> sl(_stopMutex);
            if (_stopInFlight) return;
            _stopInFlight = true;
        }

        _stop.store(true);

        // 1. Stop the mic so no new audio enters the queue.
        if (_mic) _mic->Stop();

        // 2. Wake and join the consumer thread BEFORE touching the stream.
        {
            std::lock_guard<std::mutex> lk(_audioMutex);
            _audioStop = true;
            _audioCv.notify_all();
        }
        if (_consumer.joinable()) _consumer.join();

        // 3. Join the worker thread.
        if (_worker.joinable()) _worker.join();

        // 4. Release mic handle.
        if (_mic) _mic.reset();

        // 5. Null the stream + recognizer pointers under guard.
        //    Deliberately NOT freeing them at process-exit CRT teardown — leak is safe.
        //    (Same rationale as Recognizer::Stop.)
        std::lock_guard<std::mutex> lk(_gate);
        _stream = nullptr;
        _rec    = nullptr;
    }

    void SherpaRecognizer::Restart()
    {
        // Guard against concurrent Restart() calls.
        if (_restarting.exchange(true)) {
            logger::warn("[sherpa] restart already in flight — ignoring duplicate call");
            return;
        }

        logger::info("[sherpa] restart requested");

        // Full teardown (idempotent; also works when _started is false).
        Stop();

        // Re-arm flags so Start() is accepted again.
        {
            std::lock_guard<std::mutex> sl(_stopMutex);
            _stopInFlight = false;
        }
        _stop.store(false);
        _started.store(false);

        // Clear audio queue so stale data does not bleed into the fresh session.
        {
            std::lock_guard<std::mutex> lk(_audioMutex);
            _audioQueue.clear();
            _audioStop = false;
        }
        _consecutiveFaults = 0;

        // Re-use the PhraseHandler from the original Start() call.
        if (!_onPhrase) {
            logger::error("[sherpa] restart: no phrase handler set — was Start() ever called?");
            _restarting.store(false);
            return;
        }

        // Note on _cfg / _cfgStorage: these are populated by InitEngine() and remain
        // valid across restarts (we never clear them in Stop()).  The worker thread's
        // InitEngine() call on the next Start() will rebuild them from the staged files,
        // so model paths are refreshed correctly even if files were updated on disk.
        // We do NOT preserve _rec/_stream — InitEngine() creates them fresh, which is the
        // simplest correct behaviour and avoids any use-after-free if sherpa's DLL was in
        // a degraded state.

        logger::info("[sherpa] restart: re-launching worker (InitEngine + mic re-open)");
        _started.store(true);
        _worker = std::thread([this] { WorkerLoop(); });

        logger::info("[sherpa] restarted (mic re-opened)");
        _restarting.store(false);
    }

    // =========================================================================
    // Private — waveIn MM callback path
    // =========================================================================

    void SherpaRecognizer::EnqueueAudio(const char* a_data, int a_len)
    {
        if (!a_data || a_len <= 0) return;
        std::lock_guard<std::mutex> lk(_audioMutex);
        if (_audioStop) return;
        if (static_cast<int>(_audioQueue.size()) >= kAudioQueueMax) {
            _audioQueue.pop_front();
        }
        AudioChunk chunk;
        chunk.data.assign(a_data, a_data + a_len);
        _audioQueue.push_back(std::move(chunk));
        _audioCv.notify_one();
    }

    // =========================================================================
    // Private — init (worker thread)
    // =========================================================================

    bool SherpaRecognizer::InitEngine()
    {
        const auto src  = PluginDir() / "SpeakUp";
        const auto dest = RealDestDir();
        if (dest.empty()) {
            logger::error("[sherpa] LOCALAPPDATA not set — cannot stage assets");
            return false;
        }
        logger::info("[sherpa] staging assets to {}", dest.string());
        if (!SyncTree(src, dest)) return false;
        if (_stop.load()) return false;

        // Find the sherpa model directory.
        const auto modelDir = FindSherpaModel(dest);
        if (modelDir.empty()) {
            logger::error("[sherpa] no sherpa-onnx-* directory found under {} — "
                          "install a sherpa model archive", (dest / "models").string());
            return false;
        }
        logger::info("[sherpa] model dir: {}", modelDir.string());

        // Load the DLLs from the staging root (where SyncTree copied them).
        if (std::string err = _sherpa.Load(dest.wstring()); !err.empty()) {
            logger::error("[sherpa] {}", err);
            return false;
        }
        if (_stop.load()) return false;

        // Build the paths for the int8 model files.
        const std::string mdir = modelDir.string();
        const std::string enc = mdir + "\\encoder-epoch-99-avg-1-chunk-16-left-128.int8.onnx";
        const std::string dec = mdir + "\\decoder-epoch-99-avg-1-chunk-16-left-128.int8.onnx";
        const std::string joi = mdir + "\\joiner-epoch-99-avg-1-chunk-16-left-128.int8.onnx";
        const std::string tok = mdir + "\\tokens.txt";

        // Configure the online recognizer with endpoint detection enabled.
        // Persist string backing storage in _cfgStorage so the C pointers remain
        // valid for self-heal RebuildRecognizer() calls after InitEngine() returns.
        _cfgStorage.enc = enc;
        _cfgStorage.dec = dec;
        _cfgStorage.joi = joi;
        _cfgStorage.tok = tok;

        memset(&_cfg, 0, sizeof(_cfg));
        _cfg.feat_config.sample_rate             = 16000;
        _cfg.feat_config.feature_dim             = 80;
        _cfg.model_config.transducer.encoder     = _cfgStorage.enc.c_str();
        _cfg.model_config.transducer.decoder     = _cfgStorage.dec.c_str();
        _cfg.model_config.transducer.joiner      = _cfgStorage.joi.c_str();
        _cfg.model_config.tokens                 = _cfgStorage.tok.c_str();
        _cfg.model_config.provider               = "cpu";
        _cfg.model_config.num_threads            = 1;
        _cfg.decoding_method                     = "greedy_search";
        _cfg.enable_endpoint                     = 1;
        // Trailing-silence thresholds tuned for snappy command casting (lower = less
        // delay between finishing the phrase and the cast). rule2 fires after speech +
        // a short pause; keep it low for responsiveness without cutting continuous
        // multi-word phrases. (The decode itself is ~50-100 ms; this silence wait was
        // the bulk of the perceived latency.)
        _cfg.rule1_min_trailing_silence          = 0.7f;   // hard end after 0.7 s silence
        _cfg.rule2_min_trailing_silence          = 0.45f;  // end 0.45 s after last word
        _cfg.rule3_min_utterance_length          = 20.0f;  // 20 s max utterance

        logger::info("[sherpa] creating recognizer (enc={} ...)", enc);
        _rec = SafeCreateRecognizer(_sherpa.CreateOnlineRecognizer, &_cfg);
        if (!_rec) {
            logger::error("[sherpa] SherpaOnnxCreateOnlineRecognizer returned null "
                          "(model paths wrong or faulted?)");
            return false;
        }
        if (_stop.load()) return false;

        _stream = SafeCreateStream(_sherpa.CreateOnlineStream, _rec);
        if (!_stream) {
            logger::error("[sherpa] SherpaOnnxCreateOnlineStream returned null");
            return false;
        }

        if (MicCapture::DeviceCount() == 0) {
            logger::error("[sherpa] no microphone device found — recognition unavailable");
            return false;
        }

        _mic = std::make_unique<MicCapture>(
            [this](const char* d, int n) { EnqueueAudio(d, n); });
        if (std::string err = _mic->Start(); !err.empty()) {
            logger::error("[sherpa] mic start failed: {}", err);
            return false;
        }

        _consumer = std::thread([this] { ConsumerLoop(); });

        logger::info("[sherpa] ready — mic capturing ({} device(s))", MicCapture::DeviceCount());
        return true;
    }

    // =========================================================================
    // Private — self-heal: destroy + recreate recognizer+stream from _cfg
    // =========================================================================

    void SherpaRecognizer::RebuildRecognizer()
    {
        logger::warn("[sherpa] self-heal: rebuilding recognizer after {} consecutive "
                     "decode faults", kFaultThreshold);

        const SherpaOnnxOnlineRecognizer* freshRec =
            SafeCreateRecognizer(_sherpa.CreateOnlineRecognizer, &_cfg);
        if (!freshRec) {
            logger::error("[sherpa] self-heal: CreateOnlineRecognizer faulted — staying silent");
            return;
        }
        const SherpaOnnxOnlineStream* freshStream =
            SafeCreateStream(_sherpa.CreateOnlineStream, freshRec);
        if (!freshStream) {
            logger::error("[sherpa] self-heal: CreateOnlineStream faulted — staying silent");
            // freshRec leaks here intentionally (same rationale as Stop: avoid calling
            // back into sherpa DLL if it is in a broken state).
            return;
        }

        {
            std::lock_guard<std::mutex> lk(_gate);
            // Deliberately leak old _rec/_stream — sherpa may be in a fault state
            // and calling Destroy* into a broken DLL can crash.
            _rec    = freshRec;
            _stream = freshStream;
        }

        logger::info("[sherpa] self-heal: recognizer rebuilt successfully");
        _consecutiveFaults = 0;
    }

    // =========================================================================
    // Private — worker thread: init + mic-starvation watchdog
    // =========================================================================

    void SherpaRecognizer::WorkerLoop()
    {
        if (!InitEngine()) {
            logger::error("[sherpa] init failed — sherpa recognition unavailable");
            return;
        }

        // Mic-starvation watchdog: mirrors Recognizer::WorkerLoop().
        // Reuse _audioCv (which Stop() notifies) so we wake promptly on shutdown
        // rather than sleeping the full kWatchdogSecs after stop is requested.
        while (!_stop.load()) {
            {
                std::unique_lock<std::mutex> lk(_audioMutex);
                _audioCv.wait_for(lk, std::chrono::seconds(kWatchdogSecs),
                    [this] { return _stop.load(); });
            }
            if (_stop.load()) break;
            if (_mic && _mic->IsRunning()) {
                auto lastAudio = _mic->LastAudioTime();
                auto now       = std::chrono::steady_clock::now();
                auto elapsed   = std::chrono::duration_cast<std::chrono::seconds>(
                    now - lastAudio).count();
                if (elapsed >= kWatchdogSecs) {
                    logger::warn("[sherpa] watchdog: no audio received for {} s — mic may be "
                                 "stalled (device disconnected or driver error?)", elapsed);
                }
            }
        }
    }

    // =========================================================================
    // Private — consumer thread: drain audio queue + decode + dispatch
    // =========================================================================

    void SherpaRecognizer::ConsumerLoop()
    {
        // Scratch buffer for float conversion.
        std::vector<float> floatBuf;

        for (;;) {
            // ---- Wait for audio (or shutdown) -----------------------------------
            AudioChunk chunk;
            {
                std::unique_lock<std::mutex> lk(_audioMutex);
                _audioCv.wait(lk, [this] {
                    return _audioStop || !_audioQueue.empty();
                });
                if (_audioStop && _audioQueue.empty()) return;
                chunk = std::move(_audioQueue.front());
                _audioQueue.pop_front();
            }

            // ---- Convert int16 PCM -> float[-1,1] -------------------------------
            const int nSamples = static_cast<int>(chunk.data.size()) / 2;
            if (nSamples <= 0) continue;
            floatBuf.resize(nSamples);
            const auto* s16 = reinterpret_cast<const int16_t*>(chunk.data.data());
            for (int i = 0; i < nSamples; ++i) {
                floatBuf[i] = s16[i] / 32768.0f;
            }

            // ---- Feed audio + decode + endpoint check (all under _gate) ---------
            std::string transcript;
            bool fault = false;
            {
                std::lock_guard<std::mutex> lk(_gate);
                if (!_rec || !_stream) continue;

                if (SafeAcceptWaveform(_sherpa.AcceptWaveform,
                        _stream, 16000, floatBuf.data(), nSamples) < 0) {
                    logger::warn("[sherpa] AcceptWaveform SEH fault — skipping chunk");
                    fault = true;
                }
                if (!fault) {
                    // Decode all available frames.
                    // P2 cap: bound iterations to kMaxDecodeIter; exceeding it is treated
                    // as a fault so the consumer cannot spin indefinitely on a runaway stream.
                    int iter = 0;
                    for (;;) {
                        if (iter >= kMaxDecodeIter) {
                            logger::warn("[sherpa] decode loop exceeded {} iterations — treating "
                                         "as fault and breaking", kMaxDecodeIter);
                            fault = true;
                            break;
                        }
                        int ready = SafeIsReady(_sherpa.IsOnlineStreamReady, _rec, _stream);
                        if (ready <= 0) break;  // 0 = not ready; -1 = fault (exit loop)
                        if (SafeDecode(_sherpa.DecodeOnlineStream, _rec, _stream) < 0) {
                            logger::warn("[sherpa] DecodeOnlineStream SEH fault");
                            fault = true;
                            break;
                        }
                        ++iter;
                    }

                    if (!fault) {
                        // Check for endpoint.
                        int ep = SafeIsEndpoint(_sherpa.OnlineStreamIsEndpoint, _rec, _stream);
                        if (ep < 0) {
                            logger::warn("[sherpa] IsEndpoint SEH fault");
                            fault = true;
                        } else if (ep > 0) {
                            // Get result.
                            const SherpaOnnxOnlineRecognizerResult* r =
                                SafeGetResult(_sherpa.GetOnlineStreamResult, _rec, _stream);
                            if (r) {
                                transcript = NormalizeText(r->text);
                                _sherpa.DestroyOnlineRecognizerResult(r);
                            }
                            // Reset stream for next utterance.
                            if (SafeReset(_sherpa.OnlineStreamReset, _rec, _stream) < 0) {
                                logger::warn("[sherpa] OnlineStreamReset SEH fault");
                                fault = true;
                            }
                        }
                    }
                }
            }  // release _gate

            // ---- Self-heal after consecutive faults (mirrors Recognizer) --------
            if (fault) {
                ++_consecutiveFaults;
                logger::warn("[sherpa] decode fault #{} (consecutive)", _consecutiveFaults);
                if (_consecutiveFaults >= kFaultThreshold) {
                    RebuildRecognizer();
                    // _consecutiveFaults reset inside RebuildRecognizer on success
                }
                continue;
            }
            _consecutiveFaults = 0;  // clear on any clean decode

            // ---- Dispatch phrase ------------------------------------------------
            if (!transcript.empty() && _onPhrase && !_stop.load()) {
                logger::info("[sherpa] heard '{}'", transcript);
                _onPhrase(transcript);
            }
        }
    }
}
