#pragma once

#include <string>
#include <vector>

namespace vibez::audio {

struct AudioDeviceInfo {
    std::wstring id;
    std::wstring name;
    bool isDefault{};
};

enum class AudioDeviceFlow { Capture, Render };

[[nodiscard]] std::vector<AudioDeviceInfo> EnumerateAudioDevices(AudioDeviceFlow flow, std::wstring* error = nullptr);
[[nodiscard]] std::wstring DefaultAudioDeviceId(AudioDeviceFlow flow);

} // namespace vibez::audio
