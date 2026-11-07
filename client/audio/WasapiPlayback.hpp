#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace vibez::audio {

class WasapiPlayback {
public:
    using FillCallback = std::function<void(std::span<std::int16_t> samples)>;

    WasapiPlayback();
    ~WasapiPlayback();
    WasapiPlayback(const WasapiPlayback&) = delete;
    WasapiPlayback& operator=(const WasapiPlayback&) = delete;

    bool Start(const std::wstring& deviceId, FillCallback callback, std::string& error);
    void Stop();
    [[nodiscard]] bool IsRunning() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vibez::audio
