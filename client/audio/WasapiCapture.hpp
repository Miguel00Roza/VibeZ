#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace vibez::audio {

enum class CaptureSource { Microphone, SystemLoopback };

class WasapiCapture {
public:
    using FrameCallback = std::function<void(std::span<const std::int16_t> frame, std::uint64_t timestamp)>;

    WasapiCapture();
    ~WasapiCapture();
    WasapiCapture(const WasapiCapture&) = delete;
    WasapiCapture& operator=(const WasapiCapture&) = delete;

    bool Start(const std::wstring& deviceId, FrameCallback callback, std::string& error,
               CaptureSource source = CaptureSource::Microphone);
    void Stop();
    [[nodiscard]] bool IsRunning() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vibez::audio
