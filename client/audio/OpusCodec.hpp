#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "client/audio/AudioConstants.hpp"

struct OpusEncoder;
struct OpusDecoder;

namespace vibez::audio {

class VoiceEncoder {
public:
    // music enables the music tuning for the shared computer sound stream.
    explicit VoiceEncoder(int bitrate = 24000, bool music = false);
    ~VoiceEncoder();
    VoiceEncoder(const VoiceEncoder&) = delete;
    VoiceEncoder& operator=(const VoiceEncoder&) = delete;

    [[nodiscard]] std::vector<std::uint8_t> Encode(std::span<const std::int16_t> pcm) const;

private:
    ::OpusEncoder* encoder_{};
};

class VoiceDecoder {
public:
    VoiceDecoder();
    ~VoiceDecoder();
    VoiceDecoder(const VoiceDecoder&) = delete;
    VoiceDecoder& operator=(const VoiceDecoder&) = delete;

    // Empty input asks Opus for packet-loss concealment of one 20 ms frame.
    [[nodiscard]] std::vector<std::int16_t> Decode(std::span<const std::uint8_t> packet) const;

private:
    ::OpusDecoder* decoder_{};
};

} // namespace vibez::audio
