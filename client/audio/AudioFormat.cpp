#include "client/audio/AudioFormat.hpp"

#include <algorithm>
#include <cmath>

namespace vibez::audio {

void LinearResampler::Configure(const int inputRate, const int outputRate) {
    step_ = outputRate > 0 ? static_cast<double>(inputRate) / static_cast<double>(outputRate) : 1.0;
    Reset();
}

void LinearResampler::Push(const std::span<const float> input) {
    buffer_.insert(buffer_.end(), input.begin(), input.end());
}

std::size_t LinearResampler::Available() const {
    if (buffer_.size() < 2 || step_ <= 0.0) return buffer_.empty() ? 0 : 1;
    const auto remaining = static_cast<double>(buffer_.size() - 1) - position_;
    if (remaining < 0.0) return 0;
    return static_cast<std::size_t>(remaining / step_) + 1;
}

void LinearResampler::Pull(const std::size_t frames, const std::span<float> output) {
    for (std::size_t i = 0; i < frames && i < output.size(); ++i) {
        if (buffer_.empty()) {
            output[i] = 0.0F;
            continue;
        }
        const auto index = static_cast<std::size_t>(std::max(0.0, position_));
        if (index + 1 < buffer_.size()) {
            const auto fraction = static_cast<float>(position_ - static_cast<double>(index));
            output[i] = buffer_[index] + (buffer_[index + 1] - buffer_[index]) * fraction;
            position_ += step_;
        } else {
            output[i] = buffer_[index];
            position_ += step_;
        }
    }
    const auto consumed = static_cast<std::size_t>(std::max(0.0, position_));
    if (consumed > 0) {
        const auto count = std::min(consumed, buffer_.size());
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(count));
        position_ -= static_cast<double>(count);
        if (position_ < 0.0) position_ = 0.0;
    }
}

void LinearResampler::Reset() {
    buffer_.clear();
    position_ = 0.0;
}

void DownmixToMonoFloat(const std::span<const float> interleaved, const int channels, std::vector<float>& mono) {
    if (channels <= 0) {
        mono.clear();
        return;
    }
    const auto frames = interleaved.size() / static_cast<std::size_t>(channels);
    mono.resize(frames);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        float sum = 0.0F;
        for (int channel = 0; channel < channels; ++channel) sum += interleaved[frame * static_cast<std::size_t>(channels) + static_cast<std::size_t>(channel)];
        mono[frame] = sum / static_cast<float>(channels);
    }
}

void Int16ToFloat(const std::span<const std::int16_t> input, const std::span<float> output) {
    const auto count = std::min(input.size(), output.size());
    for (std::size_t i = 0; i < count; ++i) output[i] = static_cast<float>(input[i]) / 32768.0F;
}

void FloatToInt16(const std::span<const float> input, const std::span<std::int16_t> output) {
    const auto count = std::min(input.size(), output.size());
    for (std::size_t i = 0; i < count; ++i) {
        const auto clamped = std::clamp(input[i], -1.0F, 1.0F);
        output[i] = static_cast<std::int16_t>(std::lround(clamped * 32767.0F));
    }
}

float RmsLevel(const std::span<const std::int16_t> samples) {
    if (samples.empty()) return 0.0F;
    double sum = 0.0;
    for (const auto sample : samples) {
        const auto value = static_cast<double>(sample) / 32768.0;
        sum += value * value;
    }
    return static_cast<float>(std::sqrt(sum / static_cast<double>(samples.size())));
}

void PcmFramer::Push(const std::span<const std::int16_t> samples, const FrameCallback& callback) {
    pending_.insert(pending_.end(), samples.begin(), samples.end());
    std::size_t offset = 0;
    while (pending_.size() - offset >= frameSamples_) {
        callback(std::span<const std::int16_t>(pending_.data() + offset, frameSamples_));
        offset += frameSamples_;
    }
    if (offset > 0) pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(offset));
}

void PcmFramer::Reset() { pending_.clear(); }

} // namespace vibez::audio
