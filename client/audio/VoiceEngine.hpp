#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace vibez::audio {

struct AudioConfig {
    std::wstring captureDeviceId;
    std::wstring playbackDeviceId;
    bool microphoneMuted{};
    bool systemAudioEnabled{};
    float masterVolume{1.0F};
};

struct EncodedVoiceFrame {
    std::uint32_t sequence{};
    std::uint64_t timestamp{};
    std::vector<std::uint8_t> payload;
    // True when the payload is the shared computer sound instead of the mic.
    bool systemAudio{};
};

// Owns every audio thread. Capture -> Opus -> send callback and
// receive packet -> jitter -> Opus -> mixer -> WASAPI playback.
// Voice and system audio are independent streams so the receiver can adjust
// each volume separately.
class VoiceEngine {
public:
    using SendCallback = std::function<void(const EncodedVoiceFrame&)>;

    VoiceEngine();
    ~VoiceEngine();
    VoiceEngine(const VoiceEngine&) = delete;
    VoiceEngine& operator=(const VoiceEngine&) = delete;

    bool Start(const AudioConfig& config, SendCallback send, std::string& error);
    // Recreates the WASAPI streams after a device problem without dropping the
    // network session.
    bool Restart(std::string& error);
    void Stop();
    [[nodiscard]] bool IsRunning() const noexcept;

    void SetMicrophoneMuted(bool muted);
    [[nodiscard]] bool IsMicrophoneMuted() const noexcept;
    bool SetSystemAudioEnabled(bool enabled, std::string& error);
    [[nodiscard]] bool IsSystemAudioEnabled() const noexcept;
    void SetUserVolume(std::uint32_t userId, float volume);
    void SetUserMuted(std::uint32_t userId, bool muted);
    void SetUserScreenVolume(std::uint32_t userId, float volume);
    void RemoveUser(std::uint32_t userId);
    void SubmitPacket(std::uint32_t userId, bool systemAudio, std::uint32_t sequence, std::uint64_t timestamp,
                      std::span<const std::uint8_t> payload);
    [[nodiscard]] float LocalLevel() const noexcept;
    [[nodiscard]] float UserLevel(std::uint32_t userId) const;
    [[nodiscard]] float UserScreenLevel(std::uint32_t userId) const;
    // Number of media packets accepted into the per-user jitter buffers.
    [[nodiscard]] std::uint64_t ReceivedPackets() const noexcept;
    [[nodiscard]] std::uint64_t ReceivedSystemPackets() const noexcept;
    [[nodiscard]] std::string LastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vibez::audio
