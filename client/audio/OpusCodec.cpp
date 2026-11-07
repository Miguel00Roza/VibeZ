#include "client/audio/OpusCodec.hpp"

#include <opus/opus.h>

#include <stdexcept>

namespace vibez::audio {
namespace {
void Check(const int result, const char* operation) {
    if (result != OPUS_OK) throw std::runtime_error(std::string(operation) + ": " + opus_strerror(result));
}
}

VoiceEncoder::VoiceEncoder() {
    int error{};
    encoder_ = opus_encoder_create(kSampleRate, kChannels, OPUS_APPLICATION_VOIP, &error);
    Check(error, "opus_encoder_create");
    Check(opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(24000)), "OPUS_SET_BITRATE");
    Check(opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(5)), "OPUS_SET_COMPLEXITY");
    Check(opus_encoder_ctl(encoder_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE)), "OPUS_SET_SIGNAL");
}
VoiceEncoder::~VoiceEncoder() { if (encoder_) opus_encoder_destroy(encoder_); }

std::vector<std::uint8_t> VoiceEncoder::Encode(const std::span<const std::int16_t> pcm) const {
    if (pcm.size() != kFrameSamples) throw std::invalid_argument("Opus voice frames must contain exactly 960 mono samples");
    std::vector<std::uint8_t> packet(4000);
    const auto length = opus_encode(encoder_, pcm.data(), kFrameSamples, packet.data(), static_cast<opus_int32>(packet.size()));
    if (length < 0) throw std::runtime_error(std::string("opus_encode: ") + opus_strerror(length));
    packet.resize(static_cast<std::size_t>(length));
    return packet;
}

VoiceDecoder::VoiceDecoder() {
    int error{};
    decoder_ = opus_decoder_create(kSampleRate, kChannels, &error);
    Check(error, "opus_decoder_create");
}
VoiceDecoder::~VoiceDecoder() { if (decoder_) opus_decoder_destroy(decoder_); }

std::vector<std::int16_t> VoiceDecoder::Decode(const std::span<const std::uint8_t> packet) const {
    std::vector<std::int16_t> pcm(kFrameSamples);
    const auto frames = opus_decode(decoder_, packet.empty() ? nullptr : packet.data(), static_cast<opus_int32>(packet.size()), pcm.data(), kFrameSamples, 0);
    if (frames < 0) throw std::runtime_error(std::string("opus_decode: ") + opus_strerror(frames));
    pcm.resize(static_cast<std::size_t>(frames));
    return pcm;
}
} // namespace vibez::audio
