#pragma once

#include "client/audio/VoiceEngine.hpp"
#include "client/net/NetworkClient.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace vibez::client {

// Target bitrate used to pace outgoing screen frames. The UI uses the same
// value to bound the size of each JPEG so the sender queue does not overflow.
inline constexpr double kVideoTargetBitrateBps = 4000000.0;

struct ClientSessionConfig {
    NetworkConfig network;
    audio::AudioConfig audio;
    // When audio hardware or Opus is unavailable the session still connects;
    // the caller can inspect AudioRunning()/AudioError() and inform the user.
    bool enableAudio{true};
};

// A complete JPEG screen frame reconstructed from UDP fragments.
struct VideoFrame {
    std::uint32_t sessionId{};
    std::uint32_t frameId{};
    bool keyframe{};
    std::vector<std::uint8_t> jpeg;
};

// Shared core used by both the console client and (later) the Qt UI. It owns
// the network client and the voice engine and never blocks the caller:
// commands are handed to the network/audio workers, and incoming state is
// consumed through the event queue.
class ClientSession {
public:
    ClientSession();
    ~ClientSession();
    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;

    // Returns false only when the network client cannot start. Audio failures
    // are non-fatal and reported through AudioRunning()/AudioError().
    bool Start(const ClientSessionConfig& config, std::string& error);
    void Stop();
    // Recreates the WASAPI streams without dropping the network session.
    bool RestartAudio(std::string& error);

    [[nodiscard]] bool IsRunning() const noexcept;
    [[nodiscard]] bool AudioRunning() const noexcept;
    [[nodiscard]] std::string AudioError() const;
    [[nodiscard]] ConnectionState State() const noexcept;
    [[nodiscard]] std::uint32_t SessionId() const noexcept;

    bool CreateChannel(const std::string& name);
    bool JoinChannel(std::uint32_t channelId);
    bool LeaveChannel();
    bool SetMicrophoneMuted(bool muted);
    bool SetSharing(bool sharing, std::uint16_t width, std::uint16_t height);
    bool SetSystemAudioEnabled(bool enabled, std::string& error);
    [[nodiscard]] bool SystemAudioEnabled() const noexcept;
    // Splits a JPEG frame into MTU-safe UDP packets.  The caller owns capture
    // and encoding; this core only transports and reconstructs frames.
    bool SendVideoFrame(std::span<const std::uint8_t> jpeg, bool keyframe = true);
    // Paces outgoing video to this bitrate. The UI uses the same value to bound
    // each JPEG so the sender queue never has to drop frames.
    void SetVideoBitrate(double bitrateBps) noexcept;
    bool RequestKeyframe(std::uint32_t targetSessionId);
    bool SendChat(const std::string& text);
    using VideoFrameCallback = std::function<void(const VideoFrame&)>;
    void SetVideoFrameCallback(VideoFrameCallback callback);

    void SetUserVolume(std::uint32_t userId, float volume);
    void SetUserMuted(std::uint32_t userId, bool muted);
    void SetUserScreenVolume(std::uint32_t userId, float volume);
    [[nodiscard]] float LocalLevel() const noexcept;
    [[nodiscard]] float UserLevel(std::uint32_t userId) const;
    [[nodiscard]] float UserScreenLevel(std::uint32_t userId) const;
    [[nodiscard]] std::uint64_t ReceivedPackets() const noexcept;
    [[nodiscard]] std::uint64_t ReceivedSystemPackets() const noexcept;

    [[nodiscard]] std::optional<NetworkEvent> TryPopEvent();
    [[nodiscard]] std::vector<NetworkEvent> PopEvents();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vibez::client
