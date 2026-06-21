#include "PCH.h"
#include "Recognizer.h"
#include "MicCapture.h"

#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <nlohmann/json.hpp>

namespace logger = SKSE::log;
using json = nlohmann::json;
using namespace std::chrono_literals;

namespace VSC
{
    namespace
    {
        // Scan the "models" subdirectory of the staging root for the first entry
        // matching "vosk-model-*". Returns an empty path if nothing is found.
        // This lets the same DLL work with either the small (40 MB) or the lgraph
        // (128 MB) model — whichever is installed by the user's chosen Nexus archive.
        std::filesystem::path FindModel(const std::filesystem::path& destRoot)
        {
            const auto modelsDir = destRoot / "models";
            std::error_code ec;
            if (!std::filesystem::is_directory(modelsDir, ec)) return {};

            for (const auto& entry : std::filesystem::directory_iterator(modelsDir, ec)) {
                if (ec) break;
                if (!entry.is_directory()) continue;
                const std::string name = entry.path().filename().string();
                if (name.rfind("vosk-model-", 0) == 0) {
                    return entry.path();
                }
            }
            return {};
        }

        std::filesystem::path PluginDir()
        {
            // P2 fix: check GetModuleHandleExW success; use a grow-on-
            // ERROR_INSUFFICIENT_BUFFER loop so long install paths (e.g. deep MO2
            // profile trees) do not silently truncate at MAX_PATH.
            HMODULE self = nullptr;
            if (!::GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(&PluginDir), &self)) {
                logger::error("[rec] GetModuleHandleExW failed ({})", ::GetLastError());
                return {};
            }
            std::wstring buf(MAX_PATH, L'\0');
            for (;;) {
                DWORD n = ::GetModuleFileNameW(self, buf.data(), static_cast<DWORD>(buf.size()));
                if (n == 0) {
                    logger::error("[rec] GetModuleFileNameW failed ({})", ::GetLastError());
                    return {};
                }
                if (n < static_cast<DWORD>(buf.size())) {
                    buf.resize(n);  // trim to actual length
                    break;
                }
                // Buffer was too small (return == size means truncated on Win32).
                if (buf.size() >= 32768) {
                    logger::error("[rec] GetModuleFileNameW: path exceeds 32768 chars — giving up");
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
        // is an exact MIRROR of the installed build. Without this, a model/runtime from
        // a previous build or update lingers — bloating disk and (worse) letting the
        // model picker grab a stale model. Returns the count removed.
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
                    it.disable_recursion_pending();  // remove_all handles the subtree
                }
            }
            for (const auto& o : orphans) {
                std::error_code rmEc;
                std::filesystem::remove_all(o, rmEc);
            }
            if (!orphans.empty()) {
                logger::info("[rec] staging mirror: pruned {} stale path(s) (old model/runtime)",
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
                logger::error("[rec] asset sync {} -> {} failed: {}", src.string(), dest.string(), ec.message());
                return false;
            }
            return true;
        }

        std::string BuildGrammarJson(const std::vector<std::string>& phrases)
        {
            json arr = json::array();
            for (const auto& p : phrases) {
                if (!p.empty()) arr.push_back(p);
            }
            arr.push_back("[unk]");  // absorb out-of-grammar speech
            return arr.dump();
        }

        // Parses a Vosk result. With set_words(1) the JSON carries a per-word "result"
        // array of {conf, word}; we gate on the MINIMUM word confidence (weakest link).
        // a_minConf <= 0 disables the gate (accept anything in-grammar). Always logs the
        // raw recognized text + confidence for dev testing.
        std::string ExtractText(const char* resultJson, float a_minConf)
        {
            if (!resultJson) return "";
            auto j = json::parse(resultJson, nullptr, false);
            if (j.is_discarded() || !j.contains("text") || !j["text"].is_string()) return "";
            std::string t = j["text"].get<std::string>();
            if (t.empty() || t == "[unk]") return "";

            float minConf = 1.0f;
            if (j.contains("result") && j["result"].is_array()) {
                for (const auto& w : j["result"]) {
                    if (w.contains("conf") && w["conf"].is_number()) {
                        minConf = (std::min)(minConf, w["conf"].get<float>());
                    }
                }
            }
            if (a_minConf > 0.0f && minConf < a_minConf) {
                logger::info("[rec] heard '{}' (conf {:.2f}) — REJECTED (< sensitivity {:.2f})",
                    t, minConf, a_minConf);
                return "";
            }
            logger::info("[rec] heard '{}' (conf {:.2f})", t, minConf);
            return t;
        }

        // Parses a Vosk PARTIAL result ({"partial":"..."}). Used only to measure how long
        // the best guess has been stable (custom end-of-utterance). No logging/sensitivity.
        std::string ExtractPartial(const char* partialJson)
        {
            if (!partialJson) return "";
            auto j = json::parse(partialJson, nullptr, false);
            if (j.is_discarded() || !j.contains("partial") || !j["partial"].is_string()) return "";
            return j["partial"].get<std::string>();
        }

        // CRASH FAILSAFE: build the recognizer/grammar inside a structured exception
        // handler. libvosk builds an in-memory FST and can FAULT on a pathological
        // grammar (observed: a 44k-phrase list from `psb` exhausted memory and crashed
        // the game). A fault here returns nullptr instead of tearing down the process;
        // callers keep the previous working recognizer. Isolated in its own function
        // because __try/__except can't share a frame with C++ object unwinding.
        VoskRecognizer* SafeNewGrm(VoskLoader::fn_recognizer_new_grm a_fn,
                                   VoskModel* a_model, const char* a_grammar)
        {
            __try {
                return a_fn(a_model, 16000.0f, a_grammar);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return nullptr;
            }
        }

        // Decode audio under the SAME failsafe. libvosk can fault inside the decode
        // (e.g. a CPU without the instruction set its build assumes, a corrupt model,
        // or OOM) — not only inside grammar construction.
        // Returns: >0 = utterance complete (call result()); 0 = partial; -1 = SEH fault.
        // Its own function: __try/__except can't share a frame with C++ object unwinding.
        // The returned pointer is libvosk-owned (valid until the next call on this
        // recognizer) — copy it before releasing the lock.
        int SafeDecodeEx(VoskLoader::fn_accept_waveform a_accept,
                         VoskRecognizer* a_rec, const char* a_data, int a_len)
        {
            __try {
                return a_accept(a_rec, a_data, a_len);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -1;  // fault sentinel
            }
        }

        const char* SafeResult(VoskLoader::fn_result a_result, VoskRecognizer* a_rec)
        {
            __try {
                return a_result(a_rec);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return nullptr;
            }
        }

        const char* SafeFinal(VoskLoader::fn_final_result a_final, VoskRecognizer* a_rec)
        {
            __try {
                return a_final(a_rec);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return nullptr;
            }
        }

        const char* SafePartial(VoskLoader::fn_partial_result a_fn, VoskRecognizer* a_rec)
        {
            __try {
                return a_fn(a_rec);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return nullptr;
            }
        }
    }

    // =========================================================================
    // Singleton
    // =========================================================================

    Recognizer& Recognizer::Get()
    {
        static Recognizer singleton;
        return singleton;
    }

    Recognizer::~Recognizer() { Stop(); }

    // =========================================================================
    // Public API
    // =========================================================================

    void Recognizer::Start(PhraseHandler a_onPhrase)
    {
        if (_started.exchange(true)) {
            return;
        }
        _onPhrase = std::move(a_onPhrase);
        _worker = std::thread([this] { WorkerLoop(); });
    }

    void Recognizer::SetGrammar(const std::vector<std::string>& a_phrases)
    {
        {
            std::lock_guard<std::mutex> lk(_grammarMutex);
            _desiredPhrases = a_phrases;
            _dirty = true;
        }
        _cv.notify_one();  // non-blocking; worker rebuilds off-thread
    }

    void Recognizer::Finalize()
    {
        if (!_vosk.final_result) return;
        std::string raw;
        {
            std::lock_guard<std::mutex> lock(_gate);
            if (!_rec) return;
            if (const char* j = SafeFinal(_vosk.final_result, _rec)) raw = j;
        }
        if (raw.empty()) return;
        std::string phrase = ExtractText(raw.c_str(), _sensitivity.load());
        if (!phrase.empty() && _onPhrase && !_stop.load()) {
            // Tell the consumer to discard its stale partial state so it cannot
            // fire a stability-endpoint on the same utterance we are dispatching
            // right now (double-dispatch fix P1).
            _resetEndpoint.store(true, std::memory_order_release);
            _onPhrase(phrase);
        }
    }

    void Recognizer::Stop()
    {
        if (!_started.load()) {
            return;
        }
        // Idempotent + re-armable teardown (replaces std::call_once so Restart() can
        // re-arm after a previous Stop()).  _stopMutex serialises concurrent callers;
        // only the first wins — subsequent callers return immediately once _stopInFlight
        // is cleared by Restart() (see below).
        {
            std::lock_guard<std::mutex> sl(_stopMutex);
            if (_stopInFlight) return;  // already tearing down or torn down
            _stopInFlight = true;
        }

        _stop.store(true);

        // 1. Wake the grammar-rebuild worker so it can exit.
        _cv.notify_all();

        // 2. Stop the mic FIRST so no new audio enters the queue.
        if (_mic) {
            _mic->Stop();
        }

        // 3. Wake and join the consumer thread BEFORE touching _rec.
        {
            std::lock_guard<std::mutex> lk(_audioMutex);
            _audioStop = true;
            _audioCv.notify_all();
        }
        if (_consumer.joinable()) {
            _consumer.join();
        }

        // 4. Join the grammar worker (it may be mid-rebuild — wait for it).
        if (_worker.joinable()) {
            _worker.join();
        }

        // 5. Release the mic handle.
        if (_mic) {
            _mic.reset();
        }

        // We deliberately DO NOT call recognizer_free / model_free / FreeLibrary here.
        // Stop() runs from ~Recognizer at CRT static destruction (process exit), where
        // libvosk's own mingw runtime may already have torn down — calling back into
        // it then can crash on quit. Our threads are stopped (above), so it's safe to
        // simply leak these; the OS reclaims all of it as the process exits.
        std::lock_guard<std::mutex> lock(_gate);
        _rec = nullptr;
        _model = nullptr;
    }

    void Recognizer::Restart()
    {
        // Guard against concurrent Restart() calls (two menu-close events racing).
        if (_restarting.exchange(true)) {
            logger::warn("[rec] restart already in flight — ignoring duplicate call");
            return;
        }

        logger::info("[rec] restart requested");

        // Full teardown (idempotent; also works when _started is false, i.e. the first
        // InitEngine failed — we still need to reset flags so Start() can run again).
        Stop();

        // Re-arm all flags so Start() is accepted again.
        {
            std::lock_guard<std::mutex> sl(_stopMutex);
            _stopInFlight = false;
        }
        _stop.store(false);
        _started.store(false);

        // Clear audio queue and consumer-private state so stale data from the dead
        // session does not bleed into the fresh one.
        {
            std::lock_guard<std::mutex> lk(_audioMutex);
            _audioQueue.clear();
            _audioStop = false;
        }
        _partialText.clear();
        _partialChange = {};
        _consecutiveFaults = 0;
        _resetEndpoint.store(false);

        // Re-use the PhraseHandler from the original Start() call.
        // _onPhrase is set once in Start() and not cleared by Stop(), so it is still valid.
        if (!_onPhrase) {
            logger::error("[rec] restart: no phrase handler set — was Start() ever called?");
            _restarting.store(false);
            return;
        }

        // Re-launch the worker thread (which runs InitEngine -> opens a fresh mic ->
        // spawns the consumer thread).  std::thread supports reassignment after join().
        logger::info("[rec] restart: re-launching worker (InitEngine + mic re-open)");
        _started.store(true);
        _worker = std::thread([this] { WorkerLoop(); });

        logger::info("[rec] restarted (mic re-opened)");
        _restarting.store(false);
    }

    // =========================================================================
    // Private — waveIn MM callback path (must be fast, no Vosk calls)
    // =========================================================================

    void Recognizer::EnqueueAudio(const char* a_data, int a_len)
    {
        if (!a_data || a_len <= 0) return;

        std::lock_guard<std::mutex> lk(_audioMutex);
        if (_audioStop) return;

        // Drop oldest entry when the queue is full so it can never grow unbounded.
        // This happens only if the consumer thread falls badly behind (e.g. a slow
        // Vosk build on a grammar swap).
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

    bool Recognizer::InitEngine()
    {
        const auto src  = PluginDir() / "SpeakUp";
        const auto dest = RealDestDir();
        if (dest.empty()) {
            logger::error("[rec] LOCALAPPDATA not set — cannot stage assets");
            return false;
        }
        logger::info("[rec] staging assets to {}", dest.string());
        if (!SyncTree(src, dest)) {
            return false;
        }
        if (_stop.load()) return false;  // bail early if shutting down mid-init
        if (std::string err = _vosk.Load(dest.wstring()); !err.empty()) {
            logger::error("[rec] {}", err);
            return false;
        }
        // OOV diagnostic: raise Vosk log level to 0 when debug mode is active so
        // libvosk prints which grammar words it rejects as out-of-vocabulary.
        // At -1 (default) those warnings are completely suppressed.
        const int voskLogLevel = _oovDiag.load() ? 0 : -1;
        _vosk.set_log_level(voskLogLevel);
        logger::info("[rec] vosk log level set to {} (OOV diagnostic: {})",
                     voskLogLevel, _oovDiag.load() ? "ON" : "off");
        if (_stop.load()) return false;

        // Dynamic model discovery: scan models\ for any vosk-model-* subfolder so
        // the DLL works with either the small or the lgraph Nexus archive.
        const auto model = FindModel(dest);
        if (model.empty()) {
            logger::error("[rec] no vosk-model-* directory found under {} — "
                          "install a model archive", (dest / "models").string());
            return false;
        }
        logger::info("[rec] loading model: {}", model.string());
        _model = _vosk.model_new(model.string().c_str());
        if (!_model) {
            logger::error("[rec] vosk_model_new returned null (model missing/corrupt?)");
            return false;
        }
        if (_stop.load()) return false;

        // Initial recognizer with just the [unk] sink (recognizes nothing until the
        // first real grammar arrives — no false casts in the meantime).
        {
            std::lock_guard<std::mutex> lock(_gate);
            _rec = SafeNewGrm(_vosk.recognizer_new_grm, _model, BuildGrammarJson({}).c_str());
            if (!_rec) {
                logger::error("[rec] initial recognizer_new_grm returned null");
                return false;
            }
            // Per-word confidences in results (drives the sensitivity gate + dev logging).
            if (_vosk.recognizer_set_words) _vosk.recognizer_set_words(_rec, 1);
        }

        if (MicCapture::DeviceCount() == 0) {
            logger::error("[rec] no microphone device found — recognition unavailable");
            return false;
        }

        // The mic callback now only enqueues audio — no Vosk calls in the callback.
        _mic = std::make_unique<MicCapture>([this](const char* d, int n) { EnqueueAudio(d, n); });
        if (std::string err = _mic->Start(); !err.empty()) {
            logger::error("[rec] mic start failed: {}", err);
            return false;
        }

        // Start the consumer thread that drains the audio queue.
        _consumer = std::thread([this] { ConsumerLoop(); });

        logger::info("[rec] ready — mic capturing ({} device(s))", MicCapture::DeviceCount());
        return true;
    }

    // =========================================================================
    // Private — grammar rebuild (worker thread)
    // =========================================================================

    void Recognizer::RebuildRecognizer()
    {
        // Snapshot the current desired phrases under the grammar mutex.
        std::vector<std::string> phrases;
        {
            std::lock_guard<std::mutex> lk(_grammarMutex);
            phrases = _desiredPhrases;
        }

        logger::warn("[rec] self-heal: rebuilding recognizer from current grammar "
                     "({} phrases) after {} consecutive decode faults",
                     phrases.size(), kFaultThreshold);

        std::string grammar = BuildGrammarJson(phrases);
        VoskRecognizer* fresh = SafeNewGrm(_vosk.recognizer_new_grm, _model, grammar.c_str());
        if (!fresh) {
            logger::error("[rec] self-heal: recognizer_new_grm faulted again — staying silent");
            return;
        }
        if (_vosk.recognizer_set_words) _vosk.recognizer_set_words(fresh, 1);

        VoskRecognizer* old = nullptr;
        {
            std::lock_guard<std::mutex> lock(_gate);
            old = _rec;
            _rec = fresh;
        }
        // Free old recognizer off-lock so it doesn't stall the waveIn callback path.
        if (old) _vosk.recognizer_free(old);

        logger::info("[rec] self-heal: recognizer rebuilt successfully");
        _consecutiveFaults = 0;
    }

    // =========================================================================
    // Private — consumer thread: drain audio queue + decode + endpointing
    // =========================================================================

    void Recognizer::ConsumerLoop()
    {
        auto watchdogStart = std::chrono::steady_clock::now();
        bool watchdogWarned = false;

        for (;;) {
            // ---- Wait for audio (or shutdown signal) ----------------------------
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

            // ---- Watchdog reset -------------------------------------------------
            watchdogStart  = std::chrono::steady_clock::now();
            watchdogWarned = false;

            // ---- Vosk decode (all under _gate) ----------------------------------
            // We distinguish three outcomes from SafeDecodeEx:
            //   ret  >  0: utterance endpoint — call result()
            //   ret == 0: mid-utterance partial
            //   ret == -1: SEH fault
            const char* a_data = chunk.data.data();
            const int   a_len  = static_cast<int>(chunk.data.size());

            std::string raw;
            bool faulted = false;
            {
                std::lock_guard<std::mutex> lock(_gate);
                if (!_rec) continue;

                // P1 double-dispatch fix: Finalize() may have just dispatched the
                // current utterance from another thread.  Clear our stale partial so
                // the stability endpoint cannot re-fire on the same phrase.
                if (_resetEndpoint.exchange(false, std::memory_order_acq_rel)) {
                    _partialText.clear();
                    _partialChange = {};  // reset timestamp so stability timer restarts
                }

                int decodeRet = SafeDecodeEx(_vosk.accept_waveform, _rec, a_data, a_len);

                if (decodeRet == -1) {
                    // SEH fault inside accept_waveform
                    faulted = true;
                } else if (decodeRet > 0) {
                    // Vosk's own endpoint fired
                    if (const char* j = SafeResult(_vosk.result, _rec)) {
                        raw = j;
                    }
                    _partialText.clear();
                } else {
                    // Mid-utterance: check our custom stability endpoint
                    if (const float thr = _utteranceThreshold.load();
                        thr > 0.0f && _vosk.partial_result)
                    {
                        std::string partial =
                            ExtractPartial(SafePartial(_vosk.partial_result, _rec));
                        const auto now = std::chrono::steady_clock::now();
                        if (partial != _partialText) {
                            _partialText = std::move(partial);
                            _partialChange = now;
                        } else if (!_partialText.empty()) {
                            const float stable =
                                std::chrono::duration<float>(now - _partialChange).count();
                            if (stable >= thr) {
                                if (const char* jf = SafeFinal(_vosk.final_result, _rec)) {
                                    raw = jf;
                                }
                                _partialText.clear();
                            }
                        }
                    }
                }
            }  // release _gate

            // ---- Self-heal after consecutive faults -----------------------------
            if (faulted) {
                ++_consecutiveFaults;
                logger::warn("[rec] decode fault #{} (consecutive)", _consecutiveFaults);
                if (_consecutiveFaults >= kFaultThreshold) {
                    RebuildRecognizer();
                    // _consecutiveFaults reset inside RebuildRecognizer on success
                }
                continue;
            }
            _consecutiveFaults = 0;  // clear on any successful decode

            // ---- Dispatch phrase ------------------------------------------------
            if (raw.empty()) continue;
            std::string phrase = ExtractText(raw.c_str(), _sensitivity.load());
            if (!phrase.empty() && _onPhrase && !_stop.load()) {
                _onPhrase(phrase);
            }
        }
    }

    // =========================================================================
    // Private — worker thread: init + grammar rebuilds + watchdog
    // =========================================================================

    void Recognizer::WorkerLoop()
    {
        if (!InitEngine()) {
            return;  // recognition unavailable; logged above
        }

        // Service grammar rebuilds off the main thread.
        for (;;) {
            std::vector<std::string> phrases;
            {
                std::unique_lock<std::mutex> lk(_grammarMutex);
                // Wait until there is a grammar change OR a watchdog check is due OR stop.
                _cv.wait_for(lk, std::chrono::seconds(kWatchdogSecs),
                    [this] { return _dirty || _stop.load(); });

                if (_stop.load()) return;

                if (!_dirty) {
                    // Watchdog: check whether the mic is delivering audio.
                    if (_mic && _mic->IsRunning()) {
                        auto lastAudio = _mic->LastAudioTime();
                        auto now = std::chrono::steady_clock::now();
                        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                            now - lastAudio).count();
                        if (elapsed >= kWatchdogSecs) {
                            logger::warn("[rec] watchdog: no audio received for {} s — mic may be "
                                         "stalled (device disconnected or driver error?)", elapsed);
                        }
                    }
                    continue;
                }

                // Debounce: coalesce a burst of changes (e.g. SPID applying many
                // abilities at once) into ONE rebuild. Wake early only to stop.
                _cv.wait_for(lk, 500ms, [this] { return _stop.load(); });
                if (_stop.load()) return;
                phrases = _desiredPhrases;  // latest wins
                _dirty = false;
            }

            // EXPENSIVE work off-lock and off the main thread: build the new decode
            // graph. Recognition keeps running on the old recognizer meanwhile.
            // OOV diagnostic: log phrase count so testers can correlate with any
            // libvosk OOV warnings that appear in the log when _oovDiag is active.
            if (_oovDiag.load()) {
                logger::info("[rec] OOV-diag: submitting {} phrases to Vosk grammar builder "
                             "(libvosk will warn below about any OOV words)",
                             phrases.size());
            }
            std::string grammar = BuildGrammarJson(phrases);
            VoskRecognizer* fresh = SafeNewGrm(_vosk.recognizer_new_grm, _model, grammar.c_str());
            if (!fresh) {
                logger::error("[rec] grammar build failed/faulted ({} phrases) — keeping previous "
                              "grammar (out-of-vocabulary word, or list too large for libvosk)",
                              phrases.size());
                continue;  // keep the working recognizer — never crash the game
            }
            if (_vosk.recognizer_set_words) _vosk.recognizer_set_words(fresh, 1);
            // Swap the pointer under _gate (consumer thread blocks ONLY for the swap),
            // then free the old recognizer OFF-lock so recognizer_free never stalls
            // the consumer thread or drops audio.
            VoskRecognizer* old = nullptr;
            {
                std::lock_guard<std::mutex> lock(_gate);
                old = _rec;
                _rec = fresh;
            }
            if (old) _vosk.recognizer_free(old);
            logger::info("[rec] grammar applied: {} phrases", phrases.size());
        }
    }
}
