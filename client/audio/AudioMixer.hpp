#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <unordered_map>

namespace vibez::audio {

class AudioMixer {
public:
    explicit AudioMixer(std::size_t maxSamplesPerUser = 48000);

    void PushSamples(std::uint32_t userId, std::span<const std::int16_t> samples);
    void SetUserVolume(std::uint32_t userId, float volume);
    void SetUserMuted(std::uint32_t userId, bool muted);
    void SetMasterVolume(float volume);
    void RemoveUser(std::uint32_t userId);
    void Reset();

    // Produces exactly output.size() samples. Missing users are silence.
    void Mix(std::span<std::int16_t> output);
    [[nodiscard]] float PeakLevel(std::uint32_t userId) const;

private:
    struct UserState {
        std::deque<std::int16_t> samples;
        float volume{1.0F};
        bool muted{};
        float peak{};
    };

    const std::size_t maxSamplesPerUser_;
    mutable std::mutex mutex_;
    std::unordered_map<std::uint32_t, UserState> users_;
    float masterVolume_{1.0F};
};

} // namespace vibez::audio
