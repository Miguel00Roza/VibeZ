#include "client/audio/AudioMixer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vibez::audio {

AudioMixer::AudioMixer(const std::size_t maxSamplesPerUser) : maxSamplesPerUser_(maxSamplesPerUser) {}

void AudioMixer::PushSamples(const std::uint32_t userId, const std::span<const std::int16_t> samples) {
    PushSamples(userId, false, samples);
}

void AudioMixer::PushSamples(const std::uint32_t userId, const bool systemAudio, const std::span<const std::int16_t> samples) {
    std::lock_guard lock(mutex_);
    PushSamplesLocked(Key(userId, systemAudio), samples);
}

void AudioMixer::PushSamplesLocked(const std::uint64_t key, const std::span<const std::int16_t> samples) {
    auto& stream = streams_[key];
    stream.samples.insert(stream.samples.end(), samples.begin(), samples.end());
    while (stream.samples.size() > maxSamplesPerUser_) stream.samples.pop_front();
}

void AudioMixer::SetUserVolume(const std::uint32_t userId, const float volume) {
    SetUserVolume(userId, false, volume);
}

void AudioMixer::SetUserVolume(const std::uint32_t userId, const bool systemAudio, const float volume) {
    std::lock_guard lock(mutex_);
    SetVolumeLocked(Key(userId, systemAudio), volume);
}

void AudioMixer::SetVolumeLocked(const std::uint64_t key, const float volume) {
    streams_[key].volume = std::clamp(volume, 0.0F, 2.0F);
}

void AudioMixer::SetUserMuted(const std::uint32_t userId, const bool muted) {
    SetUserMuted(userId, false, muted);
}

void AudioMixer::SetUserMuted(const std::uint32_t userId, const bool systemAudio, const bool muted) {
    std::lock_guard lock(mutex_);
    SetMutedLocked(Key(userId, systemAudio), muted);
}

void AudioMixer::SetMutedLocked(const std::uint64_t key, const bool muted) {
    auto& stream = streams_[key];
    stream.muted = muted;
    if (muted) stream.samples.clear();
}

void AudioMixer::SetMasterVolume(const float volume) {
    std::lock_guard lock(mutex_);
    masterVolume_ = std::clamp(volume, 0.0F, 2.0F);
}

void AudioMixer::RemoveUser(const std::uint32_t userId) {
    std::lock_guard lock(mutex_);
    streams_.erase(Key(userId, false));
    streams_.erase(Key(userId, true));
}

void AudioMixer::Reset() {
    std::lock_guard lock(mutex_);
    streams_.clear();
}

void AudioMixer::Mix(const std::span<std::int16_t> output) {
    std::fill(output.begin(), output.end(), std::int16_t{0});
    std::lock_guard lock(mutex_);
    for (auto& [key, stream] : streams_) {
        stream.peak = 0.0F;
        if (stream.muted) {
            stream.samples.clear();
            continue;
        }
        const auto count = std::min(output.size(), stream.samples.size());
        if (count == 0) continue;
        const auto gain = stream.volume * masterVolume_;
        float peak = 0.0F;
        for (std::size_t i = 0; i < count; ++i) {
            const auto source = static_cast<float>(stream.samples[i]) * gain;
            peak = std::max(peak, std::abs(source) / 32768.0F);
            const auto mixed = static_cast<std::int32_t>(output[i]) + static_cast<std::int32_t>(std::lround(source));
            output[i] = static_cast<std::int16_t>(std::clamp<std::int32_t>(mixed, std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max()));
        }
        stream.samples.erase(stream.samples.begin(), stream.samples.begin() + static_cast<std::ptrdiff_t>(count));
        stream.peak = std::min(peak, 1.0F);
    }
}

float AudioMixer::PeakLevel(const std::uint32_t userId) const { return PeakLevel(userId, false); }

float AudioMixer::PeakLevel(const std::uint32_t userId, const bool systemAudio) const {
    std::lock_guard lock(mutex_);
    return PeakLevelLocked(Key(userId, systemAudio));
}

float AudioMixer::PeakLevelLocked(const std::uint64_t key) const {
    const auto it = streams_.find(key);
    return it == streams_.end() ? 0.0F : it->second.peak;
}

} // namespace vibez::audio
