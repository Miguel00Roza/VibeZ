#include "server/src/ScreenStream.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace vibez::server {

std::int64_t ScreenStreamHub::NowMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void ScreenStreamHub::SyncSessions(const std::vector<SessionInfo>& sessions) {
    std::lock_guard lock(mutex_);
    std::unordered_map<std::uint32_t, SessionStream> next;
    next.reserve(sessions.size());

    for (const auto& info : sessions) {
        SessionStream stream;
        bool wasSharing = false;
        if (const auto existing = streams_.find(info.id); existing != streams_.end()) {
            wasSharing = existing->second.info.sharing;
            stream = std::move(existing->second);
        }

        if (wasSharing && !info.sharing) {
            // The participant stopped sharing: drop every pending fragment and
            // the last frame so the web page cannot display a stale screen.
            stream.pending.clear();
            stream.latest.reset();
            stream.latestGeneration = 0;
            stream.latestKeyframe = false;
            stream.lastFrameMs = 0;
            stream.fps = 0;
            stream.fpsFrames = 0;
            stream.fpsWindowStartMs = 0;
        }

        if (info.sharing) {
            if (!wasSharing) stream.sharingSinceMs = ++sharingClock_;
        } else {
            stream.sharingSinceMs = 0;
        }

        stream.info = info;
        next.emplace(info.id, std::move(stream));
    }

    streams_.swap(next);
    frameReady_.notify_all();
}

void ScreenStreamHub::ExpirePending(SessionStream& stream, const std::int64_t now) {
    for (auto it = stream.pending.begin(); it != stream.pending.end();) {
        if (now - it->second.firstSeenMs > kPendingFrameTimeoutMs) it = stream.pending.erase(it);
        else ++it;
    }
}

void ScreenStreamHub::Publish(SessionStream& stream, const PendingFrame& pending, const std::int64_t now) {
    auto frame = std::make_shared<std::vector<std::uint8_t>>();
    frame->reserve(pending.totalBytes);
    for (const auto& fragment : pending.fragments) frame->insert(frame->end(), fragment.begin(), fragment.end());

    stream.latest = std::move(frame);
    stream.latestGeneration = ++generation_;
    stream.latestKeyframe = pending.keyframe;
    stream.lastFrameMs = now;

    if (stream.fpsWindowStartMs == 0) {
        stream.fpsWindowStartMs = now;
        stream.fpsFrames = 0;
    }
    ++stream.fpsFrames;
    const auto elapsed = now - stream.fpsWindowStartMs;
    if (elapsed >= 1000) {
        stream.fps = static_cast<double>(stream.fpsFrames) * 1000.0 / static_cast<double>(elapsed);
        stream.fpsWindowStartMs = now;
        stream.fpsFrames = 0;
    }
}

void ScreenStreamHub::PushFragment(const protocol::MediaHeader& header, const std::span<const std::uint8_t> payload) {
    if (header.type != protocol::MediaType::Video || payload.empty()) return;
    if (header.fragmentCount == 0 || header.fragmentCount > protocol::kMaxVideoFragments ||
        header.fragmentIndex >= header.fragmentCount) return;

    std::lock_guard lock(mutex_);
    const auto streamIt = streams_.find(header.sessionId);
    if (streamIt == streams_.end() || !streamIt->second.info.sharing) return;
    auto& stream = streamIt->second;

    const auto now = NowMilliseconds();
    ExpirePending(stream, now);

    auto pendingIt = stream.pending.find(header.frameId);
    if (pendingIt == stream.pending.end()) {
        // Bound partial memory: four in-flight frames per participant is more
        // than the client ever keeps, and the oldest is dropped first.
        while (stream.pending.size() >= kMaxPendingFramesPerSession) {
            auto oldest = stream.pending.begin();
            for (auto candidate = stream.pending.begin(); candidate != stream.pending.end(); ++candidate) {
                if (candidate->first < oldest->first) oldest = candidate;
            }
            stream.pending.erase(oldest);
        }
        PendingFrame pending;
        pending.frameId = header.frameId;
        pending.fragmentCount = header.fragmentCount;
        pending.keyframe = header.IsKeyframe();
        pending.firstSeenMs = now;
        pending.fragments.resize(header.fragmentCount);
        pending.received.assign(header.fragmentCount, false);
        pendingIt = stream.pending.emplace(header.frameId, std::move(pending)).first;
    }

    auto& pending = pendingIt->second;
    // Datagrams for the same frame must agree on the fragment count; otherwise
    // a peer could index outside the received map of an in-flight frame.
    if (header.fragmentCount != pending.fragmentCount || header.fragmentIndex >= pending.received.size()) {
        stream.pending.erase(pendingIt);
        return;
    }
    if (pending.received[header.fragmentIndex]) return; // Duplicated datagram.
    pending.received[header.fragmentIndex] = true;
    pending.fragments[header.fragmentIndex].assign(payload.begin(), payload.end());
    pending.totalBytes += payload.size();
    ++pending.receivedCount;

    if (pending.totalBytes > protocol::kMaxVideoFrameBytes) {
        stream.pending.erase(pendingIt);
        return;
    }
    if (pending.receivedCount < pending.fragmentCount) return;

    Publish(stream, pending, now);
    for (auto it = stream.pending.begin(); it != stream.pending.end();) {
        if (it->second.frameId <= header.frameId) it = stream.pending.erase(it);
        else ++it;
    }
    frameReady_.notify_all();
}

std::uint32_t ScreenStreamHub::NewestSessionLocked() const {
    std::uint32_t newest = 0;
    std::int64_t newestSince = -1;
    for (const auto& [id, stream] : streams_) {
        if (!stream.info.sharing || !stream.latest) continue;
        if (stream.sharingSinceMs > newestSince) {
            newest = id;
            newestSince = stream.sharingSinceMs;
        }
    }
    return newest;
}

StreamFrame ScreenStreamHub::FrameLocked(const SessionStream& stream) const {
    StreamFrame frame;
    frame.sessionId = stream.info.id;
    frame.generation = stream.latestGeneration;
    frame.keyframe = stream.latestKeyframe;
    frame.jpeg = stream.latest;
    return frame;
}

std::vector<StreamInfo> ScreenStreamHub::Streams() const {
    std::lock_guard lock(mutex_);
    const auto now = NowMilliseconds();
    struct Entry {
        std::int64_t sharingSinceMs{};
        StreamInfo info;
    };
    std::vector<Entry> sharing;
    sharing.reserve(streams_.size());
    for (const auto& [id, stream] : streams_) {
        if (!stream.info.sharing) continue;
        StreamInfo info;
        info.id = stream.info.id;
        info.name = stream.info.name;
        info.channelId = stream.info.channelId;
        info.channel = stream.info.channel;
        info.width = stream.info.width;
        info.height = stream.info.height;
        // A stale frame no longer counts as live even if the session is still
        // marked as sharing (for example the sender paused its capture).
        info.fps = (stream.latest && now - stream.lastFrameMs <= 2000) ? stream.fps : 0.0;
        sharing.push_back({stream.sharingSinceMs, std::move(info)});
    }
    std::sort(sharing.begin(), sharing.end(), [](const Entry& left, const Entry& right) {
        if (left.sharingSinceMs != right.sharingSinceMs) return left.sharingSinceMs > right.sharingSinceMs;
        return left.info.id < right.info.id;
    });

    std::vector<StreamInfo> result;
    result.reserve(sharing.size());
    for (auto& entry : sharing) result.push_back(std::move(entry.info));
    return result;
}

std::optional<StreamFrame> ScreenStreamHub::LatestFrame(const std::uint32_t sessionId) const {
    std::lock_guard lock(mutex_);
    const auto id = sessionId == 0 ? NewestSessionLocked() : sessionId;
    if (id == 0) return std::nullopt;
    const auto it = streams_.find(id);
    if (it == streams_.end() || !it->second.info.sharing || !it->second.latest) return std::nullopt;
    return FrameLocked(it->second);
}

std::optional<StreamFrame> ScreenStreamHub::WaitForFrame(const std::uint32_t sessionId,
                                                        const std::uint64_t lastGeneration,
                                                        const std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    const auto ready = [&] {
        const auto id = sessionId == 0 ? NewestSessionLocked() : sessionId;
        if (id == 0) return false;
        const auto it = streams_.find(id);
        if (it == streams_.end() || !it->second.info.sharing || !it->second.latest) return false;
        return it->second.latestGeneration > lastGeneration;
    };
    if (!frameReady_.wait_for(lock, timeout, ready)) return std::nullopt;

    const auto id = sessionId == 0 ? NewestSessionLocked() : sessionId;
    if (id == 0) return std::nullopt;
    const auto it = streams_.find(id);
    if (it == streams_.end() || !it->second.latest) return std::nullopt;
    return FrameLocked(it->second);
}

std::uint32_t ScreenStreamHub::NewestSession() const {
    std::lock_guard lock(mutex_);
    return NewestSessionLocked();
}

void ScreenStreamHub::Clear() {
    std::lock_guard lock(mutex_);
    streams_.clear();
    frameReady_.notify_all();
}

} // namespace vibez::server
