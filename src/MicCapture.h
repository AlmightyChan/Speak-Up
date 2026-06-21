#pragma once

// ============================================================================
// MicCapture — default microphone -> 16 kHz / mono / 16-bit PCM via Win32 waveIn.
// Runs in-process in the plugin; raises a callback with each captured buffer on
// a waveIn worker thread (which then copies the PCM into Recognizer's queue).
//
// THREADING CONTRACT:
//   - WaveProc runs on the Win32 waveIn MM thread.
//   - EVERY WIM_DATA re-queues the buffer unconditionally so the pipeline can
//     never starve (even zero-byte or callback-throw cases are handled).
//   - _onAudio is called only when dwBytesRecorded > 0.  It must copy the data
//     into its own queue immediately; it must NOT do any expensive work here.
//
// STOP SAFETY (P0):
//   _cbInFlight tracks how many WaveProc activations are currently running.
//   Stop() sets _running=false and then spin-waits for _cbInFlight to reach 0
//   before calling waveInUnprepareHeader / waveInClose so it can never close
//   the handle while a callback is still executing.
// ============================================================================

#include <atomic>
#include <chrono>
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
            // Prime the watchdog timestamp so the worker doesn't fire immediately.
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            _lastAudioNs.store(ns, std::memory_order_relaxed);
            ::waveInStart(_hwi);
            return "";
        }

        void Stop()
        {
            if (!_hwi) return;
            // Gate new waveInAddBuffer calls before issuing the driver teardown.
            _running.store(false, std::memory_order_seq_cst);
            ::waveInStop(_hwi);
            ::waveInReset(_hwi);  // flushes queued buffers; MM thread drains outstanding WIM_DATA

            // Spin-wait until every in-flight WaveProc activation has returned.
            // Each WaveProc increments _cbInFlight at entry and decrements at exit
            // (both sides of the re-queue), so when the count reaches 0 it is safe
            // to close the handle.  waveInReset guarantees no NEW callbacks fire
            // after it returns; we are just waiting for the ones already running.
            while (_cbInFlight.load(std::memory_order_acquire) > 0) {
                ::Sleep(0);  // yield; the MM thread is short-lived, so this is fast
            }

            for (auto& h : _headers) {
                ::waveInUnprepareHeader(_hwi, &h, sizeof(WAVEHDR));
            }
            ::waveInClose(_hwi);
            _hwi = nullptr;
        }

        // Returns the time of the last WIM_DATA callback with recorded bytes.
        // Used by the watchdog to detect mic starvation.
        std::chrono::steady_clock::time_point LastAudioTime() const
        {
            const int64_t ns = _lastAudioNs.load(std::memory_order_relaxed);
            return std::chrono::steady_clock::time_point{
                std::chrono::nanoseconds{ ns }
            };
        }

        bool IsRunning() const { return _running.load(); }

    private:
        static void CALLBACK WaveProc(HWAVEIN, UINT uMsg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR)
        {
            if (uMsg != WIM_DATA) return;
            auto* self = reinterpret_cast<MicCapture*>(inst);
            if (!self) return;
            auto* hdr = reinterpret_cast<WAVEHDR*>(p1);
            if (!hdr) return;

            // Increment in-flight counter so Stop() knows this activation is live.
            self->_cbInFlight.fetch_add(1, std::memory_order_acquire);

            // Snapshot the handle atomically — Stop() may null _hwi after the drain,
            // so we must NOT read _hwi after decrementing the counter.
            HWAVEIN localHwi = self->_hwi;

            // Wrap the ENTIRE body so nothing crosses the C callback boundary.
            try {
                if (self->_running.load(std::memory_order_relaxed)) {
                    // Deliver audio to the consumer BEFORE re-queuing so the buffer
                    // stays valid for the copy inside _onAudio.
                    if (hdr->dwBytesRecorded > 0 && self->_onAudio) {
                        // Record the timestamp as nanoseconds since epoch (int64_t is
                        // trivially copyable so std::atomic is well-formed on all targets).
                        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
                        self->_lastAudioNs.store(ns, std::memory_order_relaxed);
                        self->_onAudio(hdr->lpData, static_cast<int>(hdr->dwBytesRecorded));
                    }
                }
            } catch (...) {
                // Swallow — never let an exception propagate out of the waveIn callback.
            }

            // Re-queue only when still running AND the captured handle is still valid.
            // Guard with _running check to avoid AddBuffer on a handle being torn down.
            if (localHwi && self->_running.load(std::memory_order_relaxed)) {
                hdr->dwBytesRecorded = 0;
                hdr->dwFlags &= ~WHDR_DONE;
                ::waveInAddBuffer(localHwi, hdr, sizeof(WAVEHDR));
            }

            // Decrement AFTER the re-queue attempt so Stop()'s spin sees 0 only when
            // this callback has fully finished (including the AddBuffer, if any).
            self->_cbInFlight.fetch_sub(1, std::memory_order_release);
        }

        // 8 buffers × 100 ms each = 800 ms headroom before pipeline starvation.
        static constexpr int kBuffers     = 8;
        static constexpr int kBufferBytes = 3200;  // 100 ms @ 16k/mono/16-bit

        AudioHandler                   _onAudio;
        HWAVEIN                        _hwi = nullptr;
        std::vector<std::vector<char>> _buffers;
        std::vector<WAVEHDR>           _headers;
        std::atomic<bool>              _running{ false };

        // Count of WaveProc activations currently executing (incremented at entry,
        // decremented after the optional re-queue).  Stop() spin-waits on this
        // reaching 0 before calling waveInUnprepareHeader / waveInClose.
        std::atomic<int>               _cbInFlight{ 0 };

        // Written by WaveProc (MM thread), read by watchdog (worker thread).
        // int64_t (nanoseconds since steady_clock epoch) is trivially copyable so
        // std::atomic<int64_t> is lock-free on x64 and well-formed everywhere.
        std::atomic<int64_t>           _lastAudioNs{ 0 };
    };
}
