#include "client/audio/WasapiPlayback.hpp"

#include "client/audio/AudioFormat.hpp"

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

    void Run() {
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool ownsCom = comResult == S_OK || comResult == S_FALSE;
        if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
            CompleteInitialization(false, "COM initialization failed for the playback thread");
            if (ownsCom) CoUninitialize();
            return;
        }

        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> audioClient;
        ComPtr<IAudioRenderClient> renderClient;

        auto fail = [&](const std::string& message) {
            CompleteInitialization(false, message);
            if (ownsCom) CoUninitialize();
        };

        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(enumerator.GetAddressOf()));
        if (FAILED(hr)) { fail(HResultText("CoCreateInstance(MMDeviceEnumerator)", hr)); return; }
        if (deviceId.empty()) {
            hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.GetAddressOf());
        } else {
            hr = enumerator->GetDevice(deviceId.c_str(), device.GetAddressOf());
        }
        if (FAILED(hr)) { fail(HResultText("Open playback device", hr)); return; }
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(audioClient.GetAddressOf()));
        if (FAILED(hr)) { fail(HResultText("Activate(IAudioClient)", hr)); return; }

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
        if (FAILED(hr)) { fail(HResultText("Initialize shared-mode 48 kHz mono playback", hr)); return; }
        UINT32 bufferFrames{};
        hr = audioClient->GetBufferSize(&bufferFrames);
        if (FAILED(hr) || bufferFrames == 0) { fail(HResultText("GetBufferSize(playback)", hr)); return; }
        hr = audioClient->SetEventHandle(audioEvent);
        if (FAILED(hr)) { fail(HResultText("SetEventHandle(playback)", hr)); return; }
        hr = audioClient->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(renderClient.GetAddressOf()));
        if (FAILED(hr)) { fail(HResultText("GetService(IAudioRenderClient)", hr)); return; }

        hr = audioClient->Start();
        if (FAILED(hr)) { fail(HResultText("Start(playback)", hr)); return; }

        // Fill the initial device buffer after starting; a failed prefill is
        // harmless because the event loop will fill it on the next callback.
        BYTE* initial = nullptr;
        if (SUCCEEDED(renderClient->GetBuffer(bufferFrames, &initial)) && initial) {
            std::fill_n(reinterpret_cast<std::int16_t*>(initial), static_cast<std::size_t>(bufferFrames) * kChannels, std::int16_t{0});
            renderClient->ReleaseBuffer(bufferFrames, 0);
        }

        running.store(true);
        CompleteInitialization(true);

        while (!stopping.load()) {
            const DWORD wait = WaitForSingleObject(audioEvent, 100);
            if (wait != WAIT_OBJECT_0) continue;
            UINT32 padding{};
            if (FAILED(audioClient->GetCurrentPadding(&padding))) {
                stopping.store(true);
                break;
            }
            const UINT32 available = bufferFrames > padding ? bufferFrames - padding : 0;
            if (available == 0) continue;
            BYTE* data = nullptr;
            if (FAILED(renderClient->GetBuffer(available, &data)) || !data) continue;
            const auto samples = std::span<std::int16_t>(reinterpret_cast<std::int16_t*>(data), static_cast<std::size_t>(available) * kChannels);
            if (callback) callback(samples); else std::fill(samples.begin(), samples.end(), std::int16_t{0});
            renderClient->ReleaseBuffer(available, 0);
        }

        audioClient->Stop();
        running.store(false);
        if (ownsCom) CoUninitialize();
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
    impl_->thread = std::thread([this] { impl_->Run(); });
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
