#pragma once

// ============================================================================
// VoskLoader — dynamically loads libvosk.dll and resolves the C API. Runs
// IN-PROCESS inside the SKSE plugin (WDAC blocks unsigned EXEs but allows DLL
// loads, so the recognizer lives in the DLL rather than a separate companion).
// LOAD_WITH_ALTERED_SEARCH_PATH makes libvosk's sibling mingw runtime DLLs
// (libgcc/libstdc++/libwinpthread) resolve from libvosk's own directory.
// ============================================================================

#include <string>
#include <type_traits>
#include <windows.h>

extern "C" {
    typedef struct VoskModel VoskModel;
    typedef struct VoskRecognizer VoskRecognizer;
}

namespace VSC
{
    class VoskLoader
    {
    public:
        using fn_set_log_level      = void (*)(int);
        using fn_model_new          = VoskModel * (*)(const char*);
        using fn_model_free         = void (*)(VoskModel*);
        using fn_recognizer_new_grm = VoskRecognizer * (*)(VoskModel*, float, const char*);
        using fn_accept_waveform    = int (*)(VoskRecognizer*, const char*, int);
        using fn_result             = const char* (*)(VoskRecognizer*);
        using fn_partial_result     = const char* (*)(VoskRecognizer*);
        using fn_final_result       = const char* (*)(VoskRecognizer*);
        using fn_recognizer_free    = void (*)(VoskRecognizer*);
        using fn_recognizer_set_words = void (*)(VoskRecognizer*, int);

        fn_set_log_level      set_log_level      = nullptr;
        fn_model_new          model_new          = nullptr;
        fn_model_free         model_free         = nullptr;
        fn_recognizer_new_grm recognizer_new_grm = nullptr;
        fn_accept_waveform    accept_waveform    = nullptr;
        fn_result             result             = nullptr;
        fn_partial_result     partial_result     = nullptr;
        fn_final_result       final_result       = nullptr;
        fn_recognizer_free    recognizer_free    = nullptr;
        fn_recognizer_set_words recognizer_set_words = nullptr;

        // dir = folder containing libvosk.dll (+ its mingw deps). "" on success.
        std::string Load(const std::wstring& dir)
        {
            std::wstring path = dir + L"\\libvosk.dll";
            _dll = ::LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (!_dll) {
                return "LoadLibraryEx(libvosk.dll) failed, err " + std::to_string(::GetLastError());
            }
            bool ok = true;
            auto get = [&](auto& target, const char* name) {
                target = reinterpret_cast<std::remove_reference_t<decltype(target)>>(
                    ::GetProcAddress(_dll, name));
                if (!target) ok = false;
            };
            get(set_log_level,      "vosk_set_log_level");
            get(model_new,          "vosk_model_new");
            get(model_free,         "vosk_model_free");
            get(recognizer_new_grm, "vosk_recognizer_new_grm");
            get(accept_waveform,    "vosk_recognizer_accept_waveform");
            get(result,             "vosk_recognizer_result");
            get(partial_result,     "vosk_recognizer_partial_result");
            get(final_result,       "vosk_recognizer_final_result");
            get(recognizer_free,    "vosk_recognizer_free");
            if (!ok) return "missing required vosk_* exports in libvosk.dll";
            // Optional export: present in newer libvosk builds; used for per-word confidence.
            recognizer_set_words = reinterpret_cast<fn_recognizer_set_words>(
                ::GetProcAddress(_dll, "vosk_recognizer_set_words"));
            return "";
        }

    private:
        HMODULE _dll = nullptr;
    };
}
