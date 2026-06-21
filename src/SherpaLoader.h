#pragma once

// ============================================================================
// SherpaLoader — dynamically loads sherpa-onnx-c-api.dll and onnxruntime.dll
// and resolves the C API via GetProcAddress.
//
// onnxruntime.dll is loaded FIRST (as a bare dependent) so that when
// LOAD_WITH_ALTERED_SEARCH_PATH is used for sherpa-onnx-c-api.dll the runtime
// is already in the process and Windows skips the search entirely.
//
// The c-api header is included ONLY for struct/type definitions; the dllimport
// declarations are never reached because we define SHERPA_ONNX_BUILD_MAIN_LIB
// before inclusion (so SHERPA_ONNX_API expands to dllexport, not dllimport,
// making the declarations inert — we never call them directly).
// ============================================================================

#include <string>
#include <type_traits>
#include <windows.h>

// Pull in the struct definitions from the vendored header.
// Define BUILD_MAIN_LIB so SHERPA_ONNX_API = dllexport (not dllimport) —
// this prevents the compiler from treating the forward-declared functions as
// import stubs.  We resolve every function ourselves via GetProcAddress.
#define SHERPA_ONNX_BUILD_MAIN_LIB
#include "sherpa-onnx/c-api/c-api.h"
#undef SHERPA_ONNX_BUILD_MAIN_LIB

namespace VSC
{
    class SherpaLoader
    {
    public:
        // ---- Typed function pointer declarations (from c-api.h signatures) ----
        using fn_CreateOnlineRecognizer =
            const SherpaOnnxOnlineRecognizer* (*)(const SherpaOnnxOnlineRecognizerConfig*);
        using fn_DestroyOnlineRecognizer =
            void (*)(const SherpaOnnxOnlineRecognizer*);
        using fn_CreateOnlineStream =
            const SherpaOnnxOnlineStream* (*)(const SherpaOnnxOnlineRecognizer*);
        using fn_DestroyOnlineStream =
            void (*)(const SherpaOnnxOnlineStream*);
        using fn_AcceptWaveform =
            void (*)(const SherpaOnnxOnlineStream*, int32_t, const float*, int32_t);
        using fn_IsOnlineStreamReady =
            int32_t (*)(const SherpaOnnxOnlineRecognizer*, const SherpaOnnxOnlineStream*);
        using fn_DecodeOnlineStream =
            void (*)(const SherpaOnnxOnlineRecognizer*, const SherpaOnnxOnlineStream*);
        using fn_GetOnlineStreamResult =
            const SherpaOnnxOnlineRecognizerResult* (*)(const SherpaOnnxOnlineRecognizer*,
                                                        const SherpaOnnxOnlineStream*);
        using fn_DestroyOnlineRecognizerResult =
            void (*)(const SherpaOnnxOnlineRecognizerResult*);
        using fn_OnlineStreamIsEndpoint =
            int32_t (*)(const SherpaOnnxOnlineRecognizer*, const SherpaOnnxOnlineStream*);
        using fn_OnlineStreamReset =
            void (*)(const SherpaOnnxOnlineRecognizer*, const SherpaOnnxOnlineStream*);
        using fn_OnlineStreamInputFinished =
            void (*)(const SherpaOnnxOnlineStream*);

        // ---- Resolved entry points -------------------------------------------
        fn_CreateOnlineRecognizer        CreateOnlineRecognizer        = nullptr;
        fn_DestroyOnlineRecognizer       DestroyOnlineRecognizer       = nullptr;
        fn_CreateOnlineStream            CreateOnlineStream            = nullptr;
        fn_DestroyOnlineStream           DestroyOnlineStream           = nullptr;
        fn_AcceptWaveform                AcceptWaveform                = nullptr;
        fn_IsOnlineStreamReady           IsOnlineStreamReady           = nullptr;
        fn_DecodeOnlineStream            DecodeOnlineStream            = nullptr;
        fn_GetOnlineStreamResult         GetOnlineStreamResult         = nullptr;
        fn_DestroyOnlineRecognizerResult DestroyOnlineRecognizerResult = nullptr;
        fn_OnlineStreamIsEndpoint        OnlineStreamIsEndpoint        = nullptr;
        fn_OnlineStreamReset             OnlineStreamReset             = nullptr;
        fn_OnlineStreamInputFinished     OnlineStreamInputFinished     = nullptr;

        // dir = folder containing sherpa-onnx-c-api.dll + onnxruntime.dll.
        // Returns "" on success or a human-readable error string.
        std::string Load(const std::wstring& dir)
        {
            // 1. Load onnxruntime first as a plain dependent so it resolves from
            //    the process image list when sherpa asks for it.
            std::wstring ortPath = dir + L"\\onnxruntime.dll";
            _ort = ::LoadLibraryExW(ortPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (!_ort) {
                return "LoadLibraryEx(onnxruntime.dll) failed, err " +
                       std::to_string(::GetLastError());
            }

            // 2. Load the sherpa C-API DLL; LOAD_WITH_ALTERED_SEARCH_PATH lets
            //    its own directory be searched for any remaining dependencies.
            std::wstring sherpaPath = dir + L"\\sherpa-onnx-c-api.dll";
            _dll = ::LoadLibraryExW(sherpaPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (!_dll) {
                return "LoadLibraryEx(sherpa-onnx-c-api.dll) failed, err " +
                       std::to_string(::GetLastError());
            }

            bool ok = true;
            auto get = [&](auto& target, const char* name) {
                target = reinterpret_cast<std::remove_reference_t<decltype(target)>>(
                    ::GetProcAddress(_dll, name));
                if (!target) ok = false;
            };

            get(CreateOnlineRecognizer,        "SherpaOnnxCreateOnlineRecognizer");
            get(DestroyOnlineRecognizer,       "SherpaOnnxDestroyOnlineRecognizer");
            get(CreateOnlineStream,            "SherpaOnnxCreateOnlineStream");
            get(DestroyOnlineStream,           "SherpaOnnxDestroyOnlineStream");
            get(AcceptWaveform,                "SherpaOnnxOnlineStreamAcceptWaveform");
            get(IsOnlineStreamReady,           "SherpaOnnxIsOnlineStreamReady");
            get(DecodeOnlineStream,            "SherpaOnnxDecodeOnlineStream");
            get(GetOnlineStreamResult,         "SherpaOnnxGetOnlineStreamResult");
            get(DestroyOnlineRecognizerResult, "SherpaOnnxDestroyOnlineRecognizerResult");
            get(OnlineStreamIsEndpoint,        "SherpaOnnxOnlineStreamIsEndpoint");
            get(OnlineStreamReset,             "SherpaOnnxOnlineStreamReset");
            get(OnlineStreamInputFinished,     "SherpaOnnxOnlineStreamInputFinished");

            if (!ok) return "missing required SherpaOnnx* exports in sherpa-onnx-c-api.dll";
            return "";
        }

    private:
        HMODULE _ort = nullptr;   // onnxruntime.dll
        HMODULE _dll = nullptr;   // sherpa-onnx-c-api.dll
    };
}
