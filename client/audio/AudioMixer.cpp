#include "client/audio/AudioMixer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vibez::audio {

AudioMixer::AudioMixer(const std::size_t maxSamplesPerUser) : maxSamplesPerUser_(maxSamplesPerUser) {}

void AudioMixer::PushSamples(const std::uint32_t userId, const std::span<const std::int16_t> samples) {
    std::lock_guard lock(mutex_);
    auto& user = users_[userId];
    user.samples.insert(user.samples.end(), samples.begin(), samples.end());
    while (user.samples.size() > maxSamplesPerUser_) user.samples.pop_front();
}

void AudioMixer::SetUserVolume(const std::uint32_t userId, const float volume) {
    std::lock_guard lock(mutex_);
    users_[userId].volume = std::clamp(volume, 0.0F, 2.0F);
}

void AudioMixer::SetUserMuted(const std::uint32_t userId, const bool muted) {
    std::lock_guard lock(mutex_);
    auto& user = users_[userId];
    user.muted = muted;
    if (muted) user.samples.clear();
}

void AudioMixer::SetMasterVolume(const float volume) {
    std::lock_guard lock(mutex_);
    masterVolume_ = std::clamp(volume, 0.0F, 2.0F);
}

void AudioMixer::RemoveUser(const std::uint32_t userId) {
    std::lock_guard lock(mutex_);
    users_.erase(userId);
}

void AudioMixer::Reset() {
    std::lock_guard lock(mutex_);
    users_.clear();
}

void AudioMixer::Mix(const std::span<std::int16_t> output) {
    std::fill(output.begin(), output.end(), std::int16_t{0});
    std::lock_guard lock(mutex_);
    for (auto& [id, user] : users_) {
        user.peak = 0.0F;
        if (user.muted) {
            user.samples.clear();
            continue;
        }
        const auto count = std::min(output.size(), user.samples.size());
        if (count == 0) continue;
        const auto gain = user.volume * masterVolume_;
        float peak = 0.0F;
        for (std::size_t i = 0; i < count; ++i) {
            const auto source = static_cast<float>(user.samples[i]) * gain;
            peak = std::max(peak, std::abs(source) / 32768.0F);
            const auto mixed = static_cast<std::int32_t>(output[i]) + static_cast<std::int32_t>(std::lround(source));
            output[i] = static_cast<std::int16_t>(std::clamp<std::int32_t>(mixed, std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max()));
        }
        user.samples.erase(user.samples.begin(), user.samples.begin() + static_cast<std::ptrdiff_t>(count));
        user.peak = std::min(peak, 1.0F);
    }
}

float AudioMixer::PeakLevel(const std::uint32_t userId) const {
    std::lock_guard lock(mutex_);
    const auto it = users_.find(userId);
    return it == users_.end() ? 0.0F : it->second.peak;
}

} // namespace vibez::audio
