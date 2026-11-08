#include "client/audio/WasapiPlayback.hpp"

#include "client/audio/AudioFormat.hpp"
#include "client/audio/AudioThread.hpp"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace vibez::audio {
namespace {
std::string HResultText(const char* operation, const HRESULT hr) {
    char text[9]{};
    std::snprintf(text, sizeof(text), "%08lX", static_cast<unsigned long>(hr));
    return std::string(operation) + " failed (HRESULT 0x" + text + ")";
}

constexpr DWORD kPlaybackBufferDuration = 1000000; // 100 ms in 100 ns units.
}

struct WasapiPlayback::Impl {
    std::thread thread;
    mutable std::mutex mutex;
    std::condition_variable ready;
    bool initialized{};
    bool initSucceeded{};
    std::string initError;
    std::atomic_bool running{};
    std::atomic_bool stopping{};
    HANDLE audioEvent{};
    FillCallback callback;
    std::wstring deviceId;

    ~Impl() { Stop(); }

    void CompleteInitialization(const bool success, std::string error = {}) {
        {
            std::lock_guard lock(mutex);
            initialized = true;
            initSucceeded = success;
            initError = std::move(error);
        }
        ready.notify_all();
    }

    void Stop() {
        stopping.store(true);
        if (audioEvent) SetEvent(audioEvent);
        if (thread.joinable()) thread.join();
        if (audioEvent) {
            CloseHandle(audioEvent);
            audioEvent = nullptr;
        }
        running.store(false);
    }

    void ThreadMain() {
        AudioThreadScope audioThread;
        bool firstAttempt = true;
        int backoffMs = 250;
        while (!stopping.load()) {
            bool started = false;
            (void)RunOnce(started);
            running.store(false);
            if (stopping.load()) break;
            if (!started && firstAttempt) return;
            firstAttempt = false;
            for (int waited = 0; waited < backoffMs && !stopping.load(); waited += 50) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            backoffMs = (std::min)(backoffMs * 2, 5000);
        }
    }

    bool RunOnce(bool& started) {
        started = false;
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool ownsCom = comResult == S_OK || comResult == S_FALSE;
        if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
            if (!initialized) CompleteInitialization(false, "COM initialization failed for the playback thread");
            if (ownsCom) CoUninitialize();
            return false;
        }

        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> audioClient;
        ComPtr<IAudioRenderClient> renderClient;
        bool audioStarted = false;

        auto fail = [&](const std::string& message) {
            if (!initialized) CompleteInitialization(false, message);
            if (audioClient && audioStarted) audioClient->Stop();
            if (ownsCom) CoUninitialize();
            running.store(false);
            return false;
        };

        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(enumerator.GetAddressOf()));
        if (FAILED(hr)) return fail(HResultText("CoCreateInstance(MMDeviceEnumerator)", hr));
        if (deviceId.empty()) {
            hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.GetAddressOf());
        } else {
            hr = enumerator->GetDevice(deviceId.c_str(), device.GetAddressOf());
        }
        if (FAILED(hr)) return fail(HResultText("Open playback device", hr));
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(audioClient.GetAddressOf()));
        if (FAILED(hr)) return fail(HResultText("Activate(IAudioClient)", hr));

        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = static_cast<WORD>(kChannels);
        format.nSamplesPerSec = static_cast<DWORD>(kSampleRate);
        format.wBitsPerSample = 16;
        format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        format.cbSize = 0;

        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kPlaybackBufferDuration, 0, &format, nullptr);
        if (FAILED(hr)) return fail(HResultText("Initialize shared-mode 48 kHz mono playback", hr));
        UINT32 bufferFrames{};
        hr = audioClient->GetBufferSize(&bufferFrames);
        if (FAILED(hr) || bufferFrames == 0) return fail(HResultText("GetBufferSize(playback)", hr));
        hr = audioClient->SetEventHandle(audioEvent);
        if (FAILED(hr)) return fail(HResultText("SetEventHandle(playback)", hr));
        hr = audioClient->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(renderClient.GetAddressOf()));
        if (FAILED(hr)) return fail(HResultText("GetService(IAudioRenderClient)", hr));

        hr = audioClient->Start();
        if (FAILED(hr)) return fail(HResultText("Start(playback)", hr));
        audioStarted = true;
        started = true;

        // Fill the initial device buffer after starting; a failed prefill is
        // harmless because the event loop will fill it on the next callback.
        BYTE* initial = nullptr;
        if (SUCCEEDED(renderClient->GetBuffer(bufferFrames, &initial)) && initial) {
            std::fill_n(reinterpret_cast<std::int16_t*>(initial), static_cast<std::size_t>(bufferFrames) * kChannels, std::int16_t{0});
            renderClient->ReleaseBuffer(bufferFrames, 0);
        }

        running.store(true);
        if (!initialized) CompleteInitialization(true);

        while (!stopping.load()) {
            const DWORD wait = WaitForSingleObject(audioEvent, 100);
            if (wait != WAIT_OBJECT_0) continue;
            UINT32 padding{};
            const HRESULT paddingResult = audioClient->GetCurrentPadding(&padding);
            if (FAILED(paddingResult)) {
                return fail(HResultText("GetCurrentPadding(playback)", paddingResult));
            }
            const UINT32 available = bufferFrames > padding ? bufferFrames - padding : 0;
            if (available == 0) continue;
            BYTE* data = nullptr;
            const HRESULT bufferResult = renderClient->GetBuffer(available, &data);
            if (FAILED(bufferResult) || !data) {
                return fail(HResultText("GetBuffer(playback)", FAILED(bufferResult) ? bufferResult : E_POINTER));
            }
            const auto samples = std::span<std::int16_t>(reinterpret_cast<std::int16_t*>(data), static_cast<std::size_t>(available) * kChannels);
            if (callback) callback(samples); else std::fill(samples.begin(), samples.end(), std::int16_t{0});
            renderClient->ReleaseBuffer(available, 0);
        }

        if (audioStarted) audioClient->Stop();
        running.store(false);
        if (ownsCom) CoUninitialize();
        return true;
    }
};

WasapiPlayback::WasapiPlayback() : impl_(std::make_unique<Impl>()) {}
WasapiPlayback::~WasapiPlayback() = default;

bool WasapiPlayback::Start(const std::wstring& deviceId, FillCallback callback, std::string& error) {
    impl_->Stop();
    {
        std::lock_guard lock(impl_->mutex);
        impl_->initialized = false;
        impl_->initSucceeded = false;
        impl_->initError.clear();
    }
    impl_->deviceId = deviceId;
    impl_->callback = std::move(callback);
    impl_->stopping.store(false);
    impl_->audioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!impl_->audioEvent) {
        error = "Could not create the WASAPI playback event";
        return false;
    }
    impl_->thread = std::thread([this] { impl_->ThreadMain(); });
    std::unique_lock lock(impl_->mutex);
    if (!impl_->ready.wait_for(lock, std::chrono::seconds(5), [this] { return impl_->initialized; })) {
        error = "Timed out while starting WASAPI playback";
        lock.unlock();
        impl_->Stop();
        return false;
    }
    if (!impl_->initSucceeded) {
        error = impl_->initError;
        lock.unlock();
        impl_->Stop();
        return false;
    }
    error.clear();
    return true;
}

void WasapiPlayback::Stop() { impl_->Stop(); }
bool WasapiPlayback::IsRunning() const noexcept { return impl_->running.load(); }

} // namespace vibez::audio
