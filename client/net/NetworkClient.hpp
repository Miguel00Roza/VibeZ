#pragma once

#include "client/net/NetworkEvents.hpp"
#include "common/protocol/Protocol.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <string_view>
#include <vector>

namespace vibez::client {

struct NetworkConfig {
    std::string host{"127.0.0.1"};
    std::uint16_t port{};
    std::string nickname{"Guest"};
    std::string token;
    bool autoReconnect{true};
};

// A fully validated media datagram. The payload excludes the protocol header.
struct MediaPacket {
    protocol::MediaType type{};
    std::uint32_t sessionId{};
    std::uint32_t sequence{};
    std::uint64_t timestamp{};
    std::uint8_t flags{};
    std::uint32_t frameId{};
    std::uint16_t fragmentIndex{};
    std::uint16_t fragmentCount{1};
    std::vector<std::uint8_t> payload;
};

// Owns one worker thread that performs TCP control I/O and UDP media I/O.
// Media callbacks are invoked on that worker thread and must return quickly;
// the voice/video pipeline only copies or enqueues data there.
class NetworkClient {
public:
    using MediaSink = std::function<void(const MediaPacket&)>;

    NetworkClient();
    ~NetworkClient();
    NetworkClient(const NetworkClient&) = delete;
    NetworkClient& operator=(const NetworkClient&) = delete;

    // Starts asynchronously. Returns false only for invalid configuration or
    // WSA startup failure; an unreachable server is reported through
    // StateChanged(Reconnecting) and retried when autoReconnect is set.
    bool Start(const NetworkConfig& config, std::string& error);
    void Stop();

    [[nodiscard]] bool IsRunning() const noexcept;
    [[nodiscard]] ConnectionState State() const noexcept;
    // Zero until the server accepted the Hello message.
    [[nodiscard]] std::uint32_t SessionId() const noexcept;

    // Control messages are sent on the worker-owned TCP socket. The request id
    // is assigned automatically and returned through the event queue for
    // Error events; it is not otherwise correlated here.
    bool SendControl(protocol::ControlType type, std::span<const std::uint8_t> payload = {});

    // The session id is added automatically once Hello has been accepted.
    bool SendAudio(std::uint32_t sequence, std::uint64_t timestamp, std::span<const std::uint8_t> payload);
    bool SendVideo(std::uint32_t sequence, std::uint64_t timestamp, std::uint8_t flags, std::uint32_t frameId,
                   std::uint16_t fragmentIndex, std::uint16_t fragmentCount,
                   std::span<const std::uint8_t> payload);
    bool RequestKeyframe(std::uint32_t targetSessionId);
    bool SendChat(std::string_view text);

    void SetAudioSink(MediaSink sink);
    void SetVideoSink(MediaSink sink);

    [[nodiscard]] std::optional<NetworkEvent> TryPopEvent();
    [[nodiscard]] std::vector<NetworkEvent> PopEvents();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vibez::client
