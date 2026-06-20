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
        constexpr const char* kModelRel = "models\\vosk-model-small-en-us-0.15";

        std::filesystem::path PluginDir()
        {
            HMODULE self = nullptr;
            ::GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&PluginDir), &self);
            wchar_t buf[MAX_PATH]{};
            ::GetModuleFileNameW(self, buf, MAX_PATH);
            return std::filesystem::path(buf).parent_path();
        }

        std::filesystem::path RealDestDir()
        {
            wchar_t buf[MAX_PATH]{};
            DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
            if (n == 0 || n >= MAX_PATH) return {};
            return std::filesystem::path(buf) / "SpeakUp";
        }

        bool SyncTree(const std::filesystem::path& src, const std::filesystem::path& dest)
        {
            std::error_code ec;
            std::filesystem::create_directories(dest, ec);
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

        // Decode audio under the SAME failsafe. libvosk can fault inside the decode (e.g.
        // a CPU without the instruction set its build assumes, a corrupt model, or OOM) —
        // not only inside grammar construction. Returns the result JSON (utterance
        // complete) / nullptr. Its own function: __try/__except can't share a frame with
        // C++ object unwinding. The returned pointer is libvosk-owned (valid until the
        // next call on this recognizer) — copy it before releasing the lock.
        const char* SafeDecode(VoskLoader::fn_accept_waveform a_accept, VoskLoader::fn_result a_result,
                               VoskRecognizer* a_rec, const char* a_data, int a_len)
        {
            __try {
                return a_accept(a_rec, a_data, a_len) ? a_result(a_rec) : nullptr;
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

    Recognizer& Recognizer::Get()
    {
        static Recognizer singleton;
        return singleton;
    }

    Recognizer::~Recognizer() { Stop(); }

    void Recognizer::Start(PhraseHandler a_onPhrase)
    {
        if (_started.exchange(true)) {
            return;
        }
        _onPhrase = std::move(a_onPhrase);
        _worker = std::thread([this] { WorkerLoop(); });
    }

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
        _vosk.set_log_level(-1);
        if (_stop.load()) return false;

        const auto model = dest / kModelRel;
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
        _mic = std::make_unique<MicCapture>([this](const char* d, int n) { OnAudio(d, n); });
        if (std::string err = _mic->Start(); !err.empty()) {
            logger::error("[rec] mic start failed: {}", err);
            return false;
        }
        logger::info("[rec] ready — mic capturing ({} device(s))", MicCapture::DeviceCount());
        return true;
    }

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
                _cv.wait(lk, [this] { return _dirty || _stop.load(); });
                if (_stop.load()) return;
                // Debounce: coalesce a burst of changes (e.g. SPID applying many
                // abilities at once) into ONE rebuild. Wake early only to stop.
                _cv.wait_for(lk, 500ms, [this] { return _stop.load(); });
                if (_stop.load()) return;
                phrases = _desiredPhrases;  // latest wins
                _dirty = false;
            }

            // EXPENSIVE work off-lock and off the main thread: build the new decode
            // graph. Recognition keeps running on the old recognizer meanwhile.
            std::string grammar = BuildGrammarJson(phrases);
            VoskRecognizer* fresh = SafeNewGrm(_vosk.recognizer_new_grm, _model, grammar.c_str());
            if (!fresh) {
                logger::error("[rec] grammar build failed/faulted ({} phrases) — keeping previous "
                              "grammar (out-of-vocabulary word, or list too large for libvosk)",
                              phrases.size());
                continue;  // keep the working recognizer — never crash the game
            }
            if (_vosk.recognizer_set_words) _vosk.recognizer_set_words(fresh, 1);
            // Swap the pointer under _gate (mic thread blocks ONLY for the swap),
            // then free the old recognizer OFF-lock so recognizer_free never stalls
            // the mic thread / drops audio buffers.
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

    void Recognizer::OnAudio(const char* a_data, int a_len)
    {
        // All libvosk calls are SEH-guarded and under _gate; we copy only the small raw
        // strings while holding the lock, then parse JSON off-lock (so the worker's
        // grammar swap isn't blocked for the parse). Two ways an utterance finalizes:
        //   1) Vosk's own endpoint fires (accept_waveform returns 1) -> take result().
        //   2) Our custom endpoint: the partial guess has been UNCHANGED for the
        //      configured threshold -> force final_result() now (snappier than waiting
        //      for Vosk's slower silence endpoint). The threshold is mic-thread-local
        //      state, so no extra locking.
        std::string raw;
        {
            std::lock_guard<std::mutex> lock(_gate);
            if (!_rec) return;
            if (const char* j = SafeDecode(_vosk.accept_waveform, _vosk.result, _rec, a_data, a_len)) {
                raw = j;                 // Vosk's natural endpoint
                _partialText.clear();
            } else if (const float thr = _utteranceThreshold.load(); thr > 0.0f && _vosk.partial_result) {
                std::string partial = ExtractPartial(SafePartial(_vosk.partial_result, _rec));
                const auto now = std::chrono::steady_clock::now();
                if (partial != _partialText) {
                    _partialText = std::move(partial);
                    _partialChange = now;
                } else if (!_partialText.empty()) {
                    const float stable = std::chrono::duration<float>(now - _partialChange).count();
                    if (stable >= thr) {     // guess stable long enough -> our endpoint
                        if (const char* jf = SafeFinal(_vosk.final_result, _rec)) raw = jf;
                        _partialText.clear();
                    }
                }
            }
        }
        if (raw.empty()) return;
        std::string phrase = ExtractText(raw.c_str(), _sensitivity.load());  // parse OFF-lock
        // Don't dispatch during shutdown — avoids re-entering the consumer while
        // teardown is in progress (the mic can fire between worker.join and mic.Stop).
        if (!phrase.empty() && _onPhrase && !_stop.load()) {
            _onPhrase(phrase);  // consumer marshals engine work to the main thread
        }
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
            _onPhrase(phrase);
        }
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

    void Recognizer::Stop()
    {
        if (!_started.load()) {
            return;
        }
        // Run teardown exactly once even if both an explicit Stop() and ~Recognizer
        // race (no double-join / double-free).
        std::call_once(_stopOnce, [this] {
            _stop.store(true);
            _cv.notify_all();
            if (_worker.joinable()) {
                _worker.join();      // worker fully exited (mic may have been started)
            }
            if (_mic) {
                _mic->Stop();        // quiesce the capture thread BEFORE touching _rec
                _mic.reset();
            }
            // We deliberately DO NOT call recognizer_free / model_free / FreeLibrary here.
            // Stop() runs from ~Recognizer at CRT static destruction (process exit), where
            // libvosk's own mingw runtime may already have torn down — calling back into
            // it then can crash on quit. Our threads are stopped (above), so it's safe to
            // simply leak these; the OS reclaims all of it as the process exits. (If Stop()
            // is ever wired to a real in-game shutdown hook, free them there instead.)
            std::lock_guard<std::mutex> lock(_gate);
            _rec = nullptr;
            _model = nullptr;
        });
    }
}
