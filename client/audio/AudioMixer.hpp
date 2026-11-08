#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <unordered_map>

namespace vibez::audio {

// Mixes independent voice and shared-computer-sound streams per user. Keeping
// the two streams separate lets every participant control the screen audio
// volume without changing the speaker's voice level.
class AudioMixer {
public:
    explicit AudioMixer(std::size_t maxSamplesPerUser = 48000);

    void PushSamples(std::uint32_t userId, std::span<const std::int16_t> samples);
    void PushSamples(std::uint32_t userId, bool systemAudio, std::span<const std::int16_t> samples);
    void SetUserVolume(std::uint32_t userId, float volume);
    void SetUserVolume(std::uint32_t userId, bool systemAudio, float volume);
    void SetUserMuted(std::uint32_t userId, bool muted);
    void SetUserMuted(std::uint32_t userId, bool systemAudio, bool muted);
    void SetMasterVolume(float volume);
    void RemoveUser(std::uint32_t userId);
    void Reset();

    // Produces exactly output.size() samples. Missing streams are silence.
    void Mix(std::span<std::int16_t> output);
    [[nodiscard]] float PeakLevel(std::uint32_t userId) const;
    [[nodiscard]] float PeakLevel(std::uint32_t userId, bool systemAudio) const;

private:
    struct StreamState {
        std::deque<std::int16_t> samples;
        float volume{1.0F};
        bool muted{};
        float peak{};
    };

    static std::uint64_t Key(std::uint32_t userId, bool systemAudio) {
        return (static_cast<std::uint64_t>(userId) << 1) | (systemAudio ? 1U : 0U);
    }

    void PushSamplesLocked(std::uint64_t key, std::span<const std::int16_t> samples);
    void SetVolumeLocked(std::uint64_t key, float volume);
    void SetMutedLocked(std::uint64_t key, bool muted);
    [[nodiscard]] float PeakLevelLocked(std::uint64_t key) const;

    const std::size_t maxSamplesPerUser_;
    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, StreamState> streams_;
    float masterVolume_{1.0F};
};

} // namespace vibez::audio
