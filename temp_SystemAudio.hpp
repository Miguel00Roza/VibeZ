#pragma once

#include "common/protocol/Protocol.hpp"
#include "server/src/ScreenStream.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

// OpusDecoder is only used through the declarations below, so this header does
// not force <opus.h> (and its include paths) on every translation unit.
struct OpusDecoder;

namespace vibez::server {

struct OpusDecoderDeleter {
    void operator()(OpusDecoder* decoder) const noexcept;
};

// Decodes the shared-computer audio once per participant and keeps a short
// PCM ring buffer that browser viewers can follow at their own pace.
//
// The buffer is bounded to a few seconds: a slow viewer skips ahead instead of
// growing memory or delaying audio. Gaps in the Opus sequence are concealed
// with PLC frames so packet loss does not click.
class SystemAudioHub {
public:
    // Cursor value that means "start near the live edge" for a new listener.
    static constexpr std::uint64_t kLiveCursor = ~std::uint64_t{0};

    SystemAudioHub() = default;
    ~SystemAudioHub();
    SystemAudioHub(const SystemAudioHub&) = delete;
    SystemAudioHub& operator=(const SystemAudioHub&) = delete;

    // Only participants that are sharing have an audio stream. Stopping a
    // share drops the decoder and the buffered samples immediately.
    void SyncSessions(const std::vector<SessionInfo>& sessions);

    // Called for every validated system-audio datagram relayed by the server.
    void PushPacket(const protocol::MediaHeader& header, std::span<const std::uint8_t> payload);

    // Resolves 0 to the newest sharing participant (or 0 when nobody shares).
    [[nodiscard]] std::uint32_t Resolve(std::uint32_t sessionId) const;
    [[nodiscard]] std::uint32_t NewestSession() const;

    // Blocks up to timeout until samples newer than cursor are available and
    // copies at most maxSamples of them. The cursor is absolute and survives
    // reconnects; kLiveCursor starts close to the live edge.
    bool Read(std::uint32_t sessionId, std::uint64_t& cursor, std::vector<std::int16_t>& out,
              std::size_t maxSamples, std::chrono::milliseconds timeout);

    // True when the participant sent system audio recently (last 2 s).
    [[nodiscard]] bool HasRecentAudio(std::uint32_t sessionId) const;

    void Clear();

private:
    static constexpr std::size_t kSampleRate = 48000;
    static constexpr std::size_t kRingSamples = kSampleRate * 4; // 4 seconds.
    static constexpr std::size_t kPrebufferSamples = kSampleRate / 5; // 200 ms.
    static constexpr std::size_t kMaxPacketSamples = kSampleRate / 8; // 120 ms.
    static constexpr std::uint32_t kMaxConcealedFrames = 5;
    static constexpr std::int64_t kRecentAudioMs = 2000;

    struct SessionAudio {
        std::vector<std::int16_t> ring = std::vector<std::int16_t>(kRingSamples);
        std::uint64_t written{};
        std::uint64_t base{};
        std::uint64_t sharingSince{};
        std::int64_t lastPacketMs{};
        std::uint32_t lastSequence{};
        bool hasSequence{};
        std::unique_ptr<OpusDecoder, OpusDecoderDeleter> decoder;
    };

    [[nodiscard]] static std::int64_t NowMilliseconds();
    [[nodiscard]] std::uint32_t NewestLocked() const;
    void Append(SessionAudio& session, const std::int16_t* samples, std::size_t count);
    void Decode(SessionAudio& session, const std::uint8_t* data, std::size_t size);
    void Conceal(SessionAudio& session, std::uint32_t frames);

    mutable std::mutex mutex_;
    std::condition_variable dataReady_;
    std::unordered_map<std::uint32_t, SessionAudio> streams_;
    std::uint64_t sharingClock_{};
};

} // namespace vibez::server
