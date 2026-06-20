#pragma once

// ============================================================================
// MicCapture — default microphone -> 16 kHz / mono / 16-bit PCM via Win32 waveIn.
// Runs in-process in the plugin; raises a callback with each captured buffer on
// a waveIn worker thread (which then feeds Vosk and dispatches phrases).
// ============================================================================

#include <atomic>
#include <functional>
#include <string>
#include <vector>
#include <windows.h>
#include <mmsystem.h>

namespace VSC
{
    class MicCapture
    {
    public:
        using AudioHandler = std::function<void(const char*, int)>;

        static UINT DeviceCount() { return ::waveInGetNumDevs(); }

        explicit MicCapture(AudioHandler onAudio) : _onAudio(std::move(onAudio)) {}
        ~MicCapture() { Stop(); }

        std::string Start()
        {
            WAVEFORMATEX fmt{};
            fmt.wFormatTag = WAVE_FORMAT_PCM;
            fmt.nChannels = 1;
            fmt.nSamplesPerSec = 16000;
            fmt.wBitsPerSample = 16;
            fmt.nBlockAlign = fmt.nChannels * fmt.wBitsPerSample / 8;
            fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

            MMRESULT r = ::waveInOpen(&_hwi, WAVE_MAPPER, &fmt,
                reinterpret_cast<DWORD_PTR>(&MicCapture::WaveProc),
                reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION);
            if (r != MMSYSERR_NOERROR) {
                return "waveInOpen failed: " + std::to_string(r);
            }

            _buffers.resize(kBuffers);
            _headers.resize(kBuffers);
            for (int i = 0; i < kBuffers; ++i) {
                _buffers[i].assign(kBufferBytes, 0);
                WAVEHDR& h = _headers[i];
                h = {};
                h.lpData = _buffers[i].data();
                h.dwBufferLength = kBufferBytes;
                ::waveInPrepareHeader(_hwi, &h, sizeof(WAVEHDR));
                ::waveInAddBuffer(_hwi, &h, sizeof(WAVEHDR));
            }
            _running.store(true);
            ::waveInStart(_hwi);
            return "";
        }

        void Stop()
        {
            if (!_hwi) return;
            _running.store(false);   // gate callbacks BEFORE teardown
            ::waveInStop(_hwi);
            ::waveInReset(_hwi);     // returns/flushes buffers; drains callbacks
            for (auto& h : _headers) {
                ::waveInUnprepareHeader(_hwi, &h, sizeof(WAVEHDR));
            }
            ::waveInClose(_hwi);
            _hwi = nullptr;
        }

    private:
        static void CALLBACK WaveProc(HWAVEIN, UINT uMsg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR)
        {
            if (uMsg != WIM_DATA) return;
            auto* self = reinterpret_cast<MicCapture*>(inst);
            auto* hdr = reinterpret_cast<WAVEHDR*>(p1);
            if (self && self->_running.load() && hdr->dwBytesRecorded > 0) {
                if (self->_onAudio) self->_onAudio(hdr->lpData, static_cast<int>(hdr->dwBytesRecorded));
                ::waveInAddBuffer(self->_hwi, hdr, sizeof(WAVEHDR));  // recycle
            }
        }

        static constexpr int kBuffers = 4;
        static constexpr int kBufferBytes = 3200;  // 100 ms @ 16k/mono/16-bit

        AudioHandler                   _onAudio;
        HWAVEIN                        _hwi = nullptr;
        std::vector<std::vector<char>> _buffers;
        std::vector<WAVEHDR>           _headers;
        std::atomic<bool>              _running{ false };
    };
}
