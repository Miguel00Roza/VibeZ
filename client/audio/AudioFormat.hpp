#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <span>
#include <vector>

#include "client/audio/AudioConstants.hpp"

namespace vibez::audio {

// Stateful linear resampler used only when WASAPI refuses the requested
// 48 kHz mono format. It is deliberately small and deterministic; the normal
// path lets WASAPI convert with AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM.
class LinearResampler {
public:
    void Configure(int inputRate, int outputRate);
    void Push(std::span<const float> input);
    void Pull(std::size_t frames, std::span<float> output);
    [[nodiscard]] std::size_t Available() const;
    void Reset();

private:
    std::deque<float> buffer_;
    double position_{};
    double step_{1.0};
};

void DownmixToMonoFloat(std::span<const float> interleaved, int channels, std::vector<float>& mono);
void Int16ToFloat(std::span<const std::int16_t> input, std::span<float> output);
void FloatToInt16(std::span<const float> input, std::span<std::int16_t> output);
[[nodiscard]] float RmsLevel(std::span<const std::int16_t> samples);

// Accumulates arbitrary WASAPI packet sizes into the fixed 20 ms Opus frame.
class PcmFramer {
public:
    using FrameCallback = std::function<void(std::span<const std::int16_t>)>;
    explicit PcmFramer(std::size_t frameSamples = kFrameSamples) : frameSamples_(frameSamples) {}
    void Push(std::span<const std::int16_t> samples, const FrameCallback& callback);
    void Reset();
    [[nodiscard]] std::size_t Pending() const noexcept { return pending_.size(); }

private:
    std::size_t frameSamples_;
    std::vector<std::int16_t> pending_;
};

} // namespace vibez::audio
