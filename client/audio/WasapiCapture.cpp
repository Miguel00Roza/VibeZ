#include "client/audio/WasapiCapture.hpp"

#include "client/audio/AudioFormat.hpp"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace vibez::audio {
namespace {
std::string HResultText(const char* operation, const HRESULT hr) {
    return std::string(operation) + " failed (HRESULT 0x" + [&] {
        char text[9]{};
        std::snprintf(text, sizeof(text), "%08lX", static_cast<unsigned long>(hr));
        return std::string(text);
    }() + ")";
}

constexpr DWORD kCaptureBufferDuration = 1000000; // 100 ms in 100 ns units.
}

struct WasapiCapture::Impl {
    std::thread thread;
    mutable std::mutex mutex;
    std::condition_variable ready;
    bool initialized{};
    bool initSucceeded{};
    std::string initError;
    std::atomic_bool running{};
    std::atomic_bool stopping{};
    HANDLE audioEvent{};
    FrameCallback callback;
    std::wstring deviceId;
    CaptureSource source{CaptureSource::Microphone};

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
            CompleteInitialization(false, "COM initialization failed for the capture thread");
            if (ownsCom) CoUninitialize();
            return;
        }

        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> audioClient;
        ComPtr<IAudioCaptureClient> captureClient;
        std::string error;

        auto fail = [&](const std::string& message) {
            CompleteInitialization(false, message);
            if (ownsCom) CoUninitialize();
        };

        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(enumerator.GetAddressOf()));
        if (FAILED(hr)) { fail(HResultText("CoCreateInstance(MMDeviceEnumerator)", hr)); return; }
        if (deviceId.empty()) {
            hr = enumerator->GetDefaultAudioEndpoint(source == CaptureSource::SystemLoopback ? eRender : eCapture,
                                                     eConsole, device.GetAddressOf());
        } else {
            hr = enumerator->GetDevice(deviceId.c_str(), device.GetAddressOf());
        }
        if (FAILED(hr)) { fail(HResultText("Open capture device", hr)); return; }
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

        DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        if (source == CaptureSource::SystemLoopback) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
        hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kCaptureBufferDuration, 0, &format, nullptr);
        if (FAILED(hr)) { fail(HResultText("Initialize shared-mode 48 kHz mono capture", hr)); return; }
        hr = audioClient->SetEventHandle(audioEvent);
        if (FAILED(hr)) { fail(HResultText("SetEventHandle(capture)", hr)); return; }
        hr = audioClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(captureClient.GetAddressOf()));
        if (FAILED(hr)) { fail(HResultText("GetService(IAudioCaptureClient)", hr)); return; }
        hr = audioClient->Start();
        if (FAILED(hr)) { fail(HResultText("Start(capture)", hr)); return; }

        running.store(true);
        CompleteInitialization(true);

        PcmFramer framer;
        std::uint64_t timestamp = 0;
        std::vector<std::int16_t> silence(kFrameSamples, 0);
        while (!stopping.load()) {
            const DWORD wait = WaitForSingleObject(audioEvent, 100);
            if (wait != WAIT_OBJECT_0) continue;
            while (!stopping.load()) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD bufferFlags = 0;
                const HRESULT result = captureClient->GetBuffer(&data, &frames, &bufferFlags, nullptr, nullptr);
                if (result == AUDCLNT_S_BUFFER_EMPTY || frames == 0) break;
                if (FAILED(result)) {
                    stopping.store(true);
                    break;
                }
                if (bufferFlags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    std::size_t remaining = frames;
                    while (remaining > 0 && !stopping.load()) {
                        const auto count = std::min<std::size_t>(remaining, silence.size());
                        framer.Push(std::span<const std::int16_t>(silence.data(), count), [&](const std::span<const std::int16_t> frame) {
                            if (callback) callback(frame, timestamp);
                            timestamp += frame.size();
                        });
                        remaining -= count;
                    }
                } else if (data) {
                    framer.Push(std::span<const std::int16_t>(reinterpret_cast<const std::int16_t*>(data), frames), [&](const std::span<const std::int16_t> frame) {
                        if (callback) callback(frame, timestamp);
                        timestamp += frame.size();
                    });
                }
                captureClient->ReleaseBuffer(frames);
            }
        }

        audioClient->Stop();
        running.store(false);
        if (ownsCom) CoUninitialize();
    }
};

WasapiCapture::WasapiCapture() : impl_(std::make_unique<Impl>()) {}
WasapiCapture::~WasapiCapture() = default;

bool WasapiCapture::Start(const std::wstring& deviceId, FrameCallback callback, std::string& error, const CaptureSource source) {
    impl_->Stop();
    {
        std::lock_guard lock(impl_->mutex);
        impl_->initialized = false;
        impl_->initSucceeded = false;
        impl_->initError.clear();
    }
    impl_->deviceId = deviceId;
    impl_->source = source;
    impl_->callback = std::move(callback);
    impl_->stopping.store(false);
    impl_->audioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!impl_->audioEvent) {
        error = "Could not create the WASAPI capture event";
        return false;
    }
    impl_->thread = std::thread([this] { impl_->Run(); });
    std::unique_lock lock(impl_->mutex);
    if (!impl_->ready.wait_for(lock, std::chrono::seconds(5), [this] { return impl_->initialized; })) {
        error = "Timed out while starting WASAPI capture";
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

void WasapiCapture::Stop() { impl_->Stop(); }
bool WasapiCapture::IsRunning() const noexcept { return impl_->running.load(); }

} // namespace vibez::audio
