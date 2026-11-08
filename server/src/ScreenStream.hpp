#pragma once

#include "common/protocol/Protocol.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace vibez::server {

// Public description of one participant that is currently sharing a screen.
struct SessionInfo {
    std::uint32_t id{};
    std::string name;
    std::uint32_t channelId{};
    std::string channel;
    bool sharing{};
    std::uint16_t width{};
    std::uint16_t height{};
};

// A participant as exposed to the web page. Only sharing sessions with at
// least one complete frame produce one of these.
struct StreamInfo {
    std::uint32_t id{};
    std::string name;
    std::uint32_t channelId{};
    std::string channel;
    std::uint16_t width{};
    std::uint16_t height{};
    double fps{};
};

// A complete JPEG frame plus the generation that identifies it. Consumers use
// the generation to wait for the next frame instead of polling.
struct StreamFrame {
    std::uint32_t sessionId{};
    std::uint64_t generation{};
    bool keyframe{};
    std::shared_ptr<const std::vector<std::uint8_t>> jpeg;
};

// Reassembles the JPEG fragments that the relay loop already sees and keeps
// the latest complete frame per sharing session. The web server reads frames
// from here; the relay path only pays a short mutex and a copy per fragment.
//
// The hub never trusts the network: frames are accepted only for sessions the
// server marked as sharing, incomplete frames are dropped after two seconds and
// a session can never hold more than a handful of partial frames or one full
// frame in memory.
class ScreenStreamHub {
public:
    ScreenStreamHub() = default;
    ScreenStreamHub(const ScreenStreamHub&) = delete;
    ScreenStreamHub& operator=(const ScreenStreamHub&) = delete;

    // Replaces the known participant list. Frame state for surviving sessions
    // is preserved; sessions that stop sharing lose their pending and last
    // frames, and sessions that disappear are removed entirely.
    void SyncSessions(const std::vector<SessionInfo>& sessions);

    // Called for every validated video datagram relayed by the server.
    void PushFragment(const protocol::MediaHeader& header, std::span<const std::uint8_t> payload);

    // Sharing sessions that currently have a frame, newest sharer first.
    [[nodiscard]] std::vector<StreamInfo> Streams() const;

    // Latest frame of a session, or of the newest sharer when id is 0.
    [[nodiscard]] std::optional<StreamFrame> LatestFrame(std::uint32_t sessionId) const;

    // Blocks until the selected session publishes a frame newer than
    // lastGeneration. With sessionId == 0 the newest sharer is followed, so the
    // stream survives a different participant starting to share.
    [[nodiscard]] std::optional<StreamFrame> WaitForFrame(std::uint32_t sessionId, std::uint64_t lastGeneration,
                                                          std::chrono::milliseconds timeout);

    // Newest sharing session id, or 0 when nobody is sharing.
    [[nodiscard]] std::uint32_t NewestSession() const;

    void Clear();

private:
    static constexpr std::size_t kMaxPendingFramesPerSession = 4;
    static constexpr std::int64_t kPendingFrameTimeoutMs = 2000;

    struct PendingFrame {
        std::uint32_t frameId{};
        std::uint16_t fragmentCount{};
        bool keyframe{};
        std::size_t totalBytes{};
        std::size_t receivedCount{};
        std::int64_t firstSeenMs{};
        std::vector<std::vector<std::uint8_t>> fragments;
        std::vector<bool> received;
    };

    struct SessionStream {
        SessionInfo info;
        std::unordered_map<std::uint32_t, PendingFrame> pending;
        std::shared_ptr<const std::vector<std::uint8_t>> latest;
        std::uint64_t latestGeneration{};
        bool latestKeyframe{};
        std::int64_t lastFrameMs{};
        std::int64_t sharingSinceMs{};
        std::int64_t fpsWindowStartMs{};
        std::uint32_t fpsFrames{};
        double fps{};
    };

    [[nodiscard]] static std::int64_t NowMilliseconds();
    void ExpirePending(SessionStream& stream, std::int64_t now);
    void Publish(SessionStream& stream, const PendingFrame& pending, std::int64_t now);
    [[nodiscard]] std::uint32_t NewestSessionLocked() const;
    [[nodiscard]] StreamFrame FrameLocked(const SessionStream& stream) const;

    mutable std::mutex mutex_;
    std::condition_variable frameReady_;
    std::unordered_map<std::uint32_t, SessionStream> streams_;
    std::uint64_t generation_{};
    // Monotonic counter that orders sharers by when they started sharing.
    std::int64_t sharingClock_{};
};

} // namespace vibez::server
