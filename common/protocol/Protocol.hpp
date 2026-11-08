#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vibez::protocol {

constexpr std::uint16_t kProtocolVersion = 4;
constexpr std::uint32_t kControlMagic = 0x56495A45; // "VIZE"
constexpr std::uint32_t kMediaMagic = 0x565A4D31;   // "VZM1"
constexpr std::size_t kMaxControlPayload = 64 * 1024;
constexpr std::size_t kMaxMediaPayload = 1200;
constexpr std::size_t kMediaHeaderSize = 34;
constexpr std::size_t kMaxVideoFrameBytes = 2 * 1024 * 1024;
constexpr std::size_t kMaxVideoFragments = 2048;
constexpr std::uint8_t kMediaFlagKeyframe = 0x01;
// Marks an audio datagram that carries the shared computer sound instead of
// the microphone. Two independent streams let the receiver control the voice
// and the screen-audio volume separately.
constexpr std::uint8_t kMediaFlagSystemAudio = 0x02;

enum class ControlType : std::uint8_t {
    Hello = 1, HelloAccepted = 2, Error = 3, CreateChannel = 4, RemoveChannel = 5,
    JoinChannel = 6, LeaveChannel = 7, ChannelSnapshot = 8, PresenceSnapshot = 9, SetMuted = 10,
    SetSharing = 11, RequestKeyframe = 12, ChatMessage = 13,
};

enum class MediaType : std::uint8_t { Audio = 1, Video = 2, Keepalive = 3 };

struct ControlMessage { ControlType type{}; std::uint32_t requestId{}; std::vector<std::uint8_t> payload; };

struct MediaHeader {
    MediaType type{};
    std::uint32_t sessionId{};
    std::uint32_t sequence{};
    std::uint64_t timestamp{};
    std::uint16_t payloadSize{};
    std::uint8_t flags{};
    std::uint32_t frameId{};
    std::uint16_t fragmentIndex{};
    std::uint16_t fragmentCount{1};

    [[nodiscard]] bool IsKeyframe() const noexcept { return (flags & kMediaFlagKeyframe) != 0; }
};

class Writer {
public:
    void U8(std::uint8_t value); void U16(std::uint16_t value); void U32(std::uint32_t value); void U64(std::uint64_t value);
    bool String(const std::string& value, std::size_t maxLength = 255); void Bytes(std::span<const std::uint8_t> bytes);
    [[nodiscard]] const std::vector<std::uint8_t>& Data() const noexcept;
private: std::vector<std::uint8_t> data_;
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) noexcept;
    [[nodiscard]] bool U8(std::uint8_t& value); [[nodiscard]] bool U16(std::uint16_t& value); [[nodiscard]] bool U32(std::uint32_t& value); [[nodiscard]] bool U64(std::uint64_t& value);
    [[nodiscard]] bool String(std::string& value, std::size_t maxLength = 255); [[nodiscard]] std::size_t Remaining() const noexcept;
private: std::span<const std::uint8_t> bytes_; std::size_t offset_{};
};

[[nodiscard]] std::vector<std::uint8_t> EncodeControl(const ControlMessage& message);
[[nodiscard]] std::optional<ControlMessage> DecodeControl(std::span<const std::uint8_t> packet);
[[nodiscard]] std::optional<ControlMessage> TryReadControl(std::vector<std::uint8_t>& stream);
[[nodiscard]] std::vector<std::uint8_t> EncodeMediaHeader(const MediaHeader& header);
[[nodiscard]] std::optional<MediaHeader> DecodeMediaHeader(std::span<const std::uint8_t> packet);

} // namespace vibez::protocol
