#include "server/src/SystemAudio.hpp"

#include <opus.h>

#include <algorithm>
#include <limits>

namespace vibez::server {

void OpusDecoderDeleter::operator()(OpusDecoder* decoder) const noexcept {
    if (decoder) opus_decoder_destroy(decoder);
}

SystemAudioHub::~SystemAudioHub() = default;

std::int64_t SystemAudioHub::NowMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void SystemAudioHub::SyncSessions(const std::vector<SessionInfo>& sessions) {
    std::lock_guard lock(mutex_);
    for (auto it = streams_.begin(); it != streams_.end();) {
        const bool sharing = std::any_of(sessions.begin(), sessions.end(), [&](const SessionInfo& info) {
            return info.id == it->first && info.sharing;
        });
        if (!sharing) it = streams_.erase(it);
        else ++it;
    }
    for (const auto& info : sessions) {
        if (!info.sharing) continue;
        auto& session = streams_[info.id];
        if (session.sharingSince == 0) session.sharingSince = ++sharingClock_;
    }
    dataReady_.notify_all();
}

void SystemAudioHub::PushPacket(const protocol::MediaHeader& header, const std::span<const std::uint8_t> payload) {
    if (header.type != protocol::MediaType::Audio || payload.empty()) return;
    if ((header.flags & protocol::kMediaFlagSystemAudio) == 0) return;

    std::lock_guard lock(mutex_);
    const auto it = streams_.find(header.sessionId);
    if (it == streams_.end()) return;
    auto& session = it->second;

    if (!session.decoder) {
        int error{};
        session.decoder.reset(opus_decoder_create(static_cast<opus_int32>(kSampleRate), 1, &error));
        if (!session.decoder) return;
    }

    if (session.hasSequence) {
        // uint32 subtraction keeps working across sequence wraparound.
        const auto gap = static_cast<std::int32_t>(header.sequence - session.lastSequence);
        if (gap <= 0) {
            if (gap > -48000) return; // Duplicate or reordered datagram.
            session.hasSequence = false;
        } else if (gap <= static_cast<std::int32_t>(kMaxConcealedFrames) + 1) {
            Conceal(session, static_cast<std::uint32_t>(gap - 1));
        }
    }

    session.lastSequence = header.sequence;
    session.hasSequence = true;
    Decode(session, payload.data(), payload.size());
    session.lastPacketMs = NowMilliseconds();
    dataReady_.notify_all();
}

void SystemAudioHub::Append(SessionAudio& session, const std::int16_t* samples, const std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) {
        if (session.written - session.base >= kRingSamples) ++session.base;
        session.ring[session.written % kRingSamples] = samples[index];
        ++session.written;
    }
}

void SystemAudioHub::Decode(SessionAudio& session, const std::uint8_t* data, const std::size_t size) {
    std::int16_t pcm[kMaxPacketSamples]{};
    const int decoded = opus_decode(session.decoder.get(), data, static_cast<opus_int32>(size), pcm,
                                    static_cast<int>(kMaxPacketSamples), 0);
    if (decoded > 0) {
        session.lastFrameSamples = static_cast<std::size_t>(decoded);
        Append(session, pcm, static_cast<std::size_t>(decoded));
    }
}

void SystemAudioHub::Conceal(SessionAudio& session, const std::uint32_t frames) {
    std::int16_t pcm[kMaxPacketSamples]{};
    // libopus fills the whole requested frame_size when concealing (there is no
    // packet telling it the real duration), so ask for exactly the last decoded
    // frame size instead of the maximum buffer size.
    const auto frameSamples = (std::max<std::size_t>)(session.lastFrameSamples, 120);
    for (std::uint32_t index = 0; index < frames; ++index) {
        const int decoded = opus_decode(session.decoder.get(), nullptr, 0, pcm,
                                        static_cast<int>(frameSamples), 0);
        if (decoded <= 0) break;
        Append(session, pcm, static_cast<std::size_t>(decoded));
    }
}

std::uint32_t SystemAudioHub::NewestLocked() const {
    std::uint32_t newest = 0;
    std::uint64_t newestSince = 0;
    for (const auto& [id, session] : streams_) {
        if (session.sharingSince > newestSince) {
            newest = id;
            newestSince = session.sharingSince;
        }
    }
    return newest;
}

std::uint32_t SystemAudioHub::NewestSession() const {
    std::lock_guard lock(mutex_);
    return NewestLocked();
}

std::uint32_t SystemAudioHub::Resolve(const std::uint32_t sessionId) const {
    std::lock_guard lock(mutex_);
    if (sessionId == 0) return NewestLocked();
    return streams_.find(sessionId) != streams_.end() ? sessionId : 0;
}

bool SystemAudioHub::Read(const std::uint32_t sessionId, std::uint64_t& cursor, std::vector<std::int16_t>& out,
                          const std::size_t maxSamples, const std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::uint32_t lastSession = 0;

    while (true) {
        const auto current = sessionId == 0 ? NewestLocked() : sessionId;
        if (current == 0) {
            // Nobody is sharing: wait until someone starts (or timeout).
            if (dataReady_.wait_until(lock, deadline) == std::cv_status::timeout) return false;
            continue;
        }
        if (current != lastSession) {
            // Follow a different participant: start at the live edge instead of
            // replaying the previous cursor against the new stream.
            if (lastSession != 0) cursor = kLiveCursor;
            lastSession = current;
        }

        const auto it = streams_.find(current);
        if (it == streams_.end()) {
            if (sessionId != 0) return false; // The selected participant stopped sharing.
            if (dataReady_.wait_until(lock, deadline) == std::cv_status::timeout) return false;
            continue;
        }

        auto& session = it->second;
        if (cursor == kLiveCursor) {
            cursor = session.written > kPrebufferSamples ? session.written - kPrebufferSamples : session.base;
        } else if (cursor < session.base || cursor > session.written) {
            // Too far behind (or a cursor from another stream): jump to the
            // live edge so latency never grows without bound.
            cursor = session.written > kPrebufferSamples ? session.written - kPrebufferSamples : session.base;
        }

        if (cursor < session.written) {
            const auto available = static_cast<std::size_t>(session.written - cursor);
            const auto count = (std::min)(available, maxSamples);
            out.resize(count);
            for (std::size_t index = 0; index < count; ++index) {
                out[index] = session.ring[(cursor + index) % kRingSamples];
            }
            cursor += count;
            return true;
        }

        if (dataReady_.wait_until(lock, deadline) == std::cv_status::timeout) return false;
    }
}

bool SystemAudioHub::HasRecentAudio(const std::uint32_t sessionId) const {
    std::lock_guard lock(mutex_);
    const auto id = sessionId == 0 ? NewestLocked() : sessionId;
    if (id == 0) return false;
    const auto it = streams_.find(id);
    if (it == streams_.end() || it->second.lastPacketMs == 0) return false;
    return NowMilliseconds() - it->second.lastPacketMs <= kRecentAudioMs;
}

void SystemAudioHub::Clear() {
    std::lock_guard lock(mutex_);
    streams_.clear();
    dataReady_.notify_all();
}

} // namespace vibez::server
