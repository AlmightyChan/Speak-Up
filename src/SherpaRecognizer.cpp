#include "PCH.h"
#include "SherpaRecognizer.h"
#include "MicCapture.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
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
        // Contextual biasing is wired separately via SetHotwords().
    }

    void SherpaRecognizer::SetHotwords(const std::string& a_phrases, float a_score, bool a_enabled)
    {
        {
            std::lock_guard<std::mutex> lk(_hotwordsMutex);
            _hotwordsText = a_phrases;
        }
        _hotwordsScore.store(a_score);
        _hotwordsEnabled.store(a_enabled);
        // Applied at the next InitEngine() (sherpa bakes hotwords into the recognizer at
        // creation) — same as the endpoint rules. A live change takes effect on restart.
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

        // 1. Gate new audio OUT of the queue. EnqueueAudio() drops anything once
        //    _audioStop is set, so the mic may keep firing callbacks harmlessly while we
        //    shut down — we do NOT stop the mic first (stopping it can block on a stalled
        //    driver, so that is deferred to the bounded step 3 below).
        {
            std::lock_guard<std::mutex> lk(_audioMutex);
            _audioStop = true;
            _audioCv.notify_all();
        }

        // 2. Join the consumer then the worker. Both wake on the flags above and return
        //    promptly; neither calls into the audio driver during shutdown, so these never
        //    hang on a dead device. (Consumer joins BEFORE we null the stream in step 4.)
        if (_consumer.joinable()) _consumer.join();
        if (_worker.joinable())   _worker.join();

        // 3. Tear down the mic with a BOUNDED wait (see CloseMicBounded): a stalled or
        //    disconnected audio driver — exactly the state that makes a user hit "restart
        //    recognizer" — can make waveInReset/waveInClose block indefinitely.
        CloseMicBounded();

        // 4. Null the stream + recognizer pointers under guard.
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

        // Perform the FULL teardown + re-init on a DETACHED thread. Restart() is invoked on
        // the MAIN GAME THREAD (VoiceController's ticker marshals the poll via SKSE AddTask),
        // and tearing down a stalled mic must never block — let alone freeze — the game. The
        // whole body is exception-walled so a throw in here can never reach std::terminate
        // (an uncaught exception on a background thread would take the process down).
        std::thread([this]() {
            try {
                // Full teardown (idempotent; bounded mic close — see Stop()).
                Stop();

                // Re-arm flags so the InitEngine path is accepted again.
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
                } else {
                    // _cfg / _cfgStorage persist across restarts (never cleared in Stop()), and
                    // InitEngine() rebuilds _rec, _stream and the mic FRESH on the new worker —
                    // refreshing model paths from staged files and avoiding any use-after-free if
                    // sherpa's DLL was left in a degraded state. _worker was joined in Stop(), so
                    // it is non-joinable here and this assignment is well-formed.
                    logger::info("[sherpa] restart: re-launching worker (InitEngine + mic re-open)");
                    _started.store(true);
                    _worker = std::thread([this] { WorkerLoop(); });
                    logger::info("[sherpa] restarted (mic re-opened)");
                }
            } catch (const std::exception& e) {
                logger::error("[sherpa] restart: exception during teardown/re-init: {}", e.what());
            } catch (...) {
                logger::error("[sherpa] restart: unknown exception during teardown/re-init");
            }
            _restarting.store(false);
        }).detach();
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
    // Private — microphone acquisition (worker thread)
    // =========================================================================

    void SherpaRecognizer::CloseMicBounded()
    {
        // Close + release _mic with a bounded wait. waveInReset/waveInClose (and the
        // in-flight-callback drain) can block indefinitely on a stalled/disconnected audio
        // driver. Run the close on a detached thread and wait only kMicCloseTimeoutMs. On
        // overrun we abandon it: the destructor is still executing, so the MicCapture's
        // buffers stay alive (no use-after-free) and we simply leak the handle + thread.
        if (!_mic) return;
        MicCapture* raw = _mic.release();
        auto done = std::make_shared<std::atomic<bool>>(false);
        std::thread([raw, done]() {
            try { delete raw; } catch (...) {}  // ~MicCapture -> waveIn teardown
            done->store(true, std::memory_order_release);
        }).detach();

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(kMicCloseTimeoutMs);
        while (!done->load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!done->load(std::memory_order_acquire)) {
            logger::warn("[sherpa] mic close exceeded {} ms — abandoning audio handle "
                         "(driver stalled / device disconnected?)", kMicCloseTimeoutMs);
        }
    }

    bool SherpaRecognizer::WaitForFirstAudio(int a_secs)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(a_secs);
        while (!_stop.load() && std::chrono::steady_clock::now() < deadline) {
            if (_mic && _mic->ReceivedAudio()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        return _mic && _mic->ReceivedAudio();
    }

    bool SherpaRecognizer::AcquireMic()
    {
        // The mic sometimes "doesn't stand up" on launch: waveInOpen succeeds but the
        // device delivers no audio (driver/device race, a wrong default device, or a
        // virtualized mic in a VM). Bring it up with up to kMicStartAttempts IMMEDIATE
        // attempts, each requiring REAL audio to arrive within kStartupAudioWaitSecs.
        for (int attempt = 1; attempt <= kMicStartAttempts && !_stop.load(); ++attempt) {
            if (MicCapture::DeviceCount() == 0) {
                logger::warn("[sherpa] mic attempt {}/{}: no input device present",
                             attempt, kMicStartAttempts);
            } else {
                _mic = std::make_unique<MicCapture>(
                    [this](const char* d, int n) { EnqueueAudio(d, n); });
                if (std::string err = _mic->Start(); !err.empty()) {
                    logger::warn("[sherpa] mic attempt {}/{}: start failed: {}",
                                 attempt, kMicStartAttempts, err);
                    CloseMicBounded();  // releases _mic (bounded; safe if half-opened)
                } else if (WaitForFirstAudio(kStartupAudioWaitSecs)) {
                    logger::info("[sherpa] mic up on attempt {} ({} device(s))",
                                 attempt, MicCapture::DeviceCount());
                    _micFailed.store(false);
                    return true;
                } else {
                    logger::warn("[sherpa] mic attempt {}/{}: opened but delivered no audio "
                                 "within {}s — retrying", attempt, kMicStartAttempts,
                                 kStartupAudioWaitSecs);
                    CloseMicBounded();
                }
            }
            // Brief, interruptible backoff before the next attempt.
            for (int i = 0; i < kMicRetryBackoffMs / 10 && !_stop.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        if (!_stop.load()) {
            logger::error("[sherpa] microphone did not start after {} attempts — recognition "
                          "unavailable until restart", kMicStartAttempts);
            _micFailed.store(true);  // VoiceController polls this for a graceful notice
        }
        return false;
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
        // modified_beam_search keeps several hypotheses per step instead of committing to
        // the single best token (greedy), which is noticeably more accurate on multi-word
        // phrases and proper nouns (spell names, dragon words) for a negligible CPU cost.
        // max_active_paths is the beam width; 0 (from the memset) would be invalid, so set
        // the sherpa default of 4 explicitly.
        _cfg.decoding_method                     = "modified_beam_search";
        _cfg.max_active_paths                    = 4;
        _cfg.enable_endpoint                     = 1;
        // Trailing-silence thresholds ("responsiveness"/"sensitivity") — lower = less
        // delay between finishing the phrase and the cast. rule2 fires after speech + a
        // short pause; keep it low for responsiveness without cutting continuous
        // multi-word phrases. (The decode itself is ~50-100 ms; this silence wait was the
        // bulk of the perceived latency.) Values come from MCM/INI via SetEndpointRules();
        // defaults mirror the previous hardcoded values. Clamp to sane bounds so a bad INI
        // can't wedge recognition (e.g. a 0 s pause that never finalizes).
        const float epR1 = std::clamp(_endpointRule1.load(), 0.10f, 3.0f);
        const float epR2 = std::clamp(_endpointRule2.load(), 0.10f, 3.0f);
        const float epR3 = std::clamp(_endpointRule3.load(), 5.0f, 120.0f);
        _cfg.rule1_min_trailing_silence          = epR1;   // hard end after this much silence
        _cfg.rule2_min_trailing_silence          = epR2;   // end this long after last word
        _cfg.rule3_min_utterance_length          = epR3;   // max utterance length
        logger::info("[sherpa] endpoint rules: hard={:.2f}s pause={:.2f}s maxUtt={:.1f}s",
                     epR1, epR2, epR3);

        // Contextual biasing (hotwords): bias the decoder toward the live spell/shout roster.
        // ONLY enabled when (a) the option is on AND (b) a bpe.vocab sits beside the model
        // (sherpa needs it to tokenize the phrases). Otherwise we leave it off and recognition
        // is unchanged. Requires modified_beam_search (set above).
        bool hotwordsOn = false;
        if (_hotwordsEnabled.load()) {
            const std::string bpeVocabPath = mdir + "\\bpe.vocab";
            std::error_code ec;
            if (std::filesystem::exists(bpeVocabPath, ec)) {
                std::string hw;
                { std::lock_guard<std::mutex> lk(_hotwordsMutex); hw = _hotwordsText; }
                if (!hw.empty()) {
                    _cfgStorage.bpeVocab = bpeVocabPath;
                    _cfgStorage.hotwords = std::move(hw);
                    _cfg.model_config.modeling_unit = "bpe";
                    _cfg.model_config.bpe_vocab     = _cfgStorage.bpeVocab.c_str();
                    _cfg.hotwords_buf               = _cfgStorage.hotwords.c_str();
                    _cfg.hotwords_buf_size          = static_cast<int32_t>(_cfgStorage.hotwords.size());
                    _cfg.hotwords_score             = _hotwordsScore.load();
                    hotwordsOn = true;
                    logger::info("[sherpa] hotwords: ON ({} bytes, score {:.2f})",
                                 _cfgStorage.hotwords.size(), _cfg.hotwords_score);
                }
            } else {
                logger::warn("[sherpa] hotwords requested but bpe.vocab not found at {} — "
                             "skipping (recognition unchanged)", bpeVocabPath);
            }
        }

        logger::info("[sherpa] creating recognizer (decoding={}, hotwords={}, enc={} ...)",
                     _cfg.decoding_method, hotwordsOn ? "on" : "off", enc);
        _rec = SafeCreateRecognizer(_sherpa.CreateOnlineRecognizer, &_cfg);
        if (!_rec && hotwordsOn) {
            // Hotwords (or a bad bpe.vocab) may have broken creation — retry WITHOUT them so
            // we keep beam search rather than dropping all the way to greedy.
            logger::warn("[sherpa] recognizer creation failed with hotwords — retrying without");
            _cfg.model_config.modeling_unit = nullptr;
            _cfg.model_config.bpe_vocab     = nullptr;
            _cfg.hotwords_buf               = nullptr;
            _cfg.hotwords_buf_size          = 0;
            hotwordsOn = false;
            _rec = SafeCreateRecognizer(_sherpa.CreateOnlineRecognizer, &_cfg);
        }
        if (!_rec) {
            // Auto-fallback: if the model/runtime rejects modified_beam_search, drop to
            // greedy_search (always supported) so recognition still comes up rather than
            // dying. Beam search is only an accuracy win; greedy is the safe baseline.
            logger::warn("[sherpa] recognizer creation failed with '{}' — falling back to "
                         "greedy_search", _cfg.decoding_method);
            _cfg.decoding_method = "greedy_search";
            _rec = SafeCreateRecognizer(_sherpa.CreateOnlineRecognizer, &_cfg);
        }
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

        // Bring the mic up with immediate retries (sets _micFailed + returns false if it
        // never delivers audio after all attempts). The recognizer + stream created above
        // are reused across mic attempts, so only the flaky device acquisition retries.
        if (!AcquireMic()) {
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
