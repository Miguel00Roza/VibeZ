#include "client/core/ClientSession.hpp"

#include "client/audio/BoundedQueue.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <thread>
#include <unordered_map>
#include <utility>

namespace vibez::client {
namespace {
void SetAudioError(std::mutex& mutex, std::string& target, std::string text) {
    std::lock_guard lock(mutex);
    target = std::move(text);
}

std::uint64_t NowMilliseconds() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

constexpr std::size_t kVideoPayloadBytes = 1200;
constexpr std::size_t kMaxPendingVideoFrames = 32;
constexpr std::uint64_t kPendingVideoTimeoutMs = 2000;
// The sender paces video to a target bitrate instead of dumping a whole frame
// into the socket. This keeps UDP buffers from overflowing and lets audio
// share the link. The value can be changed at runtime from the sharing dialog.
constexpr double kDefaultVideoBitrateBps = kVideoTargetBitrateBps;
} // namespace

struct ClientSession::Impl {
    struct PendingVideoFrame {
        std::uint32_t frameId{};
        std::uint16_t fragmentCount{};
        bool keyframe{};
        std::uint64_t startedMs{};
        std::uint64_t lastMs{};
        std::size_t totalBytes{};
        std::vector<std::vector<std::uint8_t>> fragments;
        std::vector<bool> received;
        std::uint16_t receivedCount{};
    };

    struct VideoJob {
        std::vector<std::uint8_t> jpeg;
        bool keyframe{};
    };

    ClientSessionConfig config;
    NetworkClient network;
    audio::VoiceEngine voice;
    std::atomic_bool audioRunning{};
    mutable std::mutex audioErrorMutex;
    std::string audioError;
    std::atomic_uint32_t nextVideoSequence{1};
    std::atomic_uint32_t nextVideoFrameId{1};
    std::atomic<double> videoBitrateBps{kDefaultVideoBitrateBps};
    std::mutex videoMutex;
    std::unordered_map<std::uint64_t, PendingVideoFrame> pendingVideo;
    std::deque<std::uint64_t> pendingOrder;
    VideoFrameCallback videoCallback;
    audio::BlockingQueue<VideoJob> videoQueue{2};
    std::thread videoThread;
    std::atomic_bool videoStopping{};

    static std::uint64_t VideoKey(const std::uint32_t sessionId, const std::uint32_t frameId) {
        return (static_cast<std::uint64_t>(sessionId) << 32) | frameId;
    }

    void DropPending(const std::uint64_t key) {
        pendingVideo.erase(key);
        pendingOrder.erase(std::remove(pendingOrder.begin(), pendingOrder.end(), key), pendingOrder.end());
    }

    void SubmitVideo(const MediaPacket& packet) {
        if (packet.type != protocol::MediaType::Video || packet.fragmentCount == 0 ||
            packet.fragmentCount > protocol::kMaxVideoFragments || packet.fragmentIndex >= packet.fragmentCount ||
            packet.payload.empty()) return;

        const auto now = NowMilliseconds();
        const auto key = VideoKey(packet.sessionId, packet.frameId);
        VideoFrame complete;
        VideoFrameCallback callback;
        {
            std::lock_guard lock(videoMutex);

            // Drop stale partial frames so a remote sender cannot grow memory.
            for (auto it = pendingOrder.begin(); it != pendingOrder.end();) {
                const auto entry = pendingVideo.find(*it);
                if (entry == pendingVideo.end() || now - entry->second.lastMs > kPendingVideoTimeoutMs) {
                    if (entry != pendingVideo.end()) pendingVideo.erase(entry);
                    it = pendingOrder.erase(it);
                } else {
                    ++it;
                }
            }

            auto [it, inserted] = pendingVideo.try_emplace(key);
            auto& pending = it->second;
            if (inserted) {
                pending.frameId = packet.frameId;
                pending.fragmentCount = packet.fragmentCount;
                pending.startedMs = now;
                pending.fragments.resize(packet.fragmentCount);
                pending.received.assign(packet.fragmentCount, false);
                pendingOrder.push_back(key);
                while (pendingOrder.size() > kMaxPendingVideoFrames) {
                    DropPending(pendingOrder.front());
                }
            } else if (pending.fragmentCount != packet.fragmentCount) {
                pendingVideo.erase(it);
                pendingOrder.erase(std::remove(pendingOrder.begin(), pendingOrder.end(), key), pendingOrder.end());
                return;
            }
            pending.lastMs = now;
            pending.keyframe = pending.keyframe || (packet.flags & protocol::kMediaFlagKeyframe) != 0;
            if (!pending.received[packet.fragmentIndex]) {
                pending.totalBytes += packet.payload.size();
                if (pending.totalBytes > protocol::kMaxVideoFrameBytes) {
                    DropPending(key);
                    return;
                }
                pending.fragments[packet.fragmentIndex] = packet.payload;
                pending.received[packet.fragmentIndex] = true;
                ++pending.receivedCount;
            }
            if (pending.receivedCount != pending.fragmentCount) return;

            complete.sessionId = packet.sessionId;
            complete.frameId = pending.frameId;
            complete.keyframe = pending.keyframe;
            complete.jpeg.reserve(pending.totalBytes);
            for (const auto& fragment : pending.fragments) complete.jpeg.insert(complete.jpeg.end(), fragment.begin(), fragment.end());

            // Older frames are useless once a newer one completed, but only
            // within the same sender: different participants share the frame
            // id space independently.
            for (auto orderIt = pendingOrder.begin(); orderIt != pendingOrder.end();) {
                const auto entry = pendingVideo.find(*orderIt);
                const bool sameSender = static_cast<std::uint32_t>(*orderIt >> 32) == complete.sessionId;
                if (entry != pendingVideo.end() && sameSender && entry->second.frameId <= complete.frameId) {
                    orderIt = pendingOrder.erase(orderIt);
                    pendingVideo.erase(entry);
                } else {
                    ++orderIt;
                }
            }
            callback = videoCallback;
        }
        if (callback) callback(complete);
    }

    void StartVideoThread() {
        videoStopping.store(false);
        videoQueue.Reopen();
        videoQueue.Clear();
        videoThread = std::thread([this] {
            const auto fragmentBytes = kVideoPayloadBytes;
            while (!videoStopping.load()) {
                VideoJob job;
                if (!videoQueue.WaitPop(job, std::chrono::milliseconds(100))) continue;
                if (videoStopping.load()) break;
                if (job.jpeg.empty() || job.jpeg.size() > protocol::kMaxVideoFrameBytes) continue;

                const auto fragmentCount = (job.jpeg.size() + fragmentBytes - 1) / fragmentBytes;
                if (fragmentCount == 0 || fragmentCount > protocol::kMaxVideoFragments) continue;
                const auto frameId = nextVideoFrameId.fetch_add(1);
                const auto timestamp = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
                const auto flags = static_cast<std::uint8_t>(job.keyframe ? protocol::kMediaFlagKeyframe : 0);
                const auto bitrate = std::max(500000.0, videoBitrateBps.load());

                auto nextSend = std::chrono::steady_clock::now();
                for (std::size_t index = 0; index < fragmentCount && !videoStopping.load(); ++index) {
                    const auto offset = index * fragmentBytes;
                    const auto bytes = std::min(fragmentBytes, job.jpeg.size() - offset);
                    if (!network.SendVideo(nextVideoSequence.fetch_add(1), timestamp, flags, frameId,
                                           static_cast<std::uint16_t>(index), static_cast<std::uint16_t>(fragmentCount),
                                           std::span<const std::uint8_t>(job.jpeg.data() + offset, bytes))) {
                        break;
                    }
                    const auto duration = std::chrono::duration<double>(
                        static_cast<double>(bytes * 8) / bitrate);
                    nextSend += std::chrono::duration_cast<std::chrono::steady_clock::duration>(duration);
                    auto now = std::chrono::steady_clock::now();
                    while (now < nextSend && !videoStopping.load()) {
                        const auto remaining = nextSend - now;
                        const auto slice = std::min<std::chrono::steady_clock::duration>(remaining, std::chrono::milliseconds(2));
                        std::this_thread::sleep_for(slice);
                        now = std::chrono::steady_clock::now();
                    }
                }
            }
        });
    }

    void StopVideoThread() {
        videoStopping.store(true);
        videoQueue.Close();
        if (videoThread.joinable()) videoThread.join();
        videoQueue.Clear();
    }
};

ClientSession::ClientSession() : impl_(std::make_unique<Impl>()) {}
ClientSession::~ClientSession() { Stop(); }

bool ClientSession::Start(const ClientSessionConfig& config, std::string& error) {
    Stop();
    impl_->config = config;

    std::string networkError;
    if (!impl_->network.Start(config.network, networkError)) {
        error = networkError;
        return false;
    }

    impl_->network.SetAudioSink([this](const MediaPacket& packet) {
        if (packet.type != protocol::MediaType::Audio || !impl_->audioRunning.load()) return;
        const bool systemAudio = (packet.flags & protocol::kMediaFlagSystemAudio) != 0;
        impl_->voice.SubmitPacket(packet.sessionId, systemAudio, packet.sequence, packet.timestamp, packet.payload);
    });
    impl_->network.SetVideoSink([this](const MediaPacket& packet) { impl_->SubmitVideo(packet); });

    impl_->StartVideoThread();

    impl_->audioRunning.store(false);
    if (!config.enableAudio) {
        SetAudioError(impl_->audioErrorMutex, impl_->audioError, "Audio disabled by configuration");
    } else {
        std::string audioError;
        if (impl_->voice.Start(config.audio, [this](const audio::EncodedVoiceFrame& frame) {
                (void)impl_->network.SendAudio(frame.sequence, frame.timestamp, frame.payload, frame.systemAudio);
            }, audioError)) {
            impl_->voice.SetMicrophoneMuted(config.audio.microphoneMuted);
            SetAudioError(impl_->audioErrorMutex, impl_->audioError, {});
            impl_->audioRunning.store(true);
        } else {
            SetAudioError(impl_->audioErrorMutex, impl_->audioError, std::move(audioError));
        }
    }

    error.clear();
    return true;
}

void ClientSession::Stop() {
    impl_->audioRunning.store(false);
    impl_->StopVideoThread();
    impl_->network.Stop();
    impl_->voice.Stop();
}

bool ClientSession::RestartAudio(std::string& error) {
    if (!impl_->config.enableAudio) {
        error = "Audio disabled by configuration";
        SetAudioError(impl_->audioErrorMutex, impl_->audioError, error);
        return false;
    }
    impl_->audioRunning.store(false);
    if (!impl_->voice.Restart(error)) {
        SetAudioError(impl_->audioErrorMutex, impl_->audioError, error);
        return false;
    }
    impl_->voice.SetMicrophoneMuted(impl_->config.audio.microphoneMuted);
    SetAudioError(impl_->audioErrorMutex, impl_->audioError, {});
    impl_->audioRunning.store(true);
    return true;
}

bool ClientSession::IsRunning() const noexcept { return impl_->network.IsRunning(); }
bool ClientSession::AudioRunning() const noexcept { return impl_->audioRunning.load(); }
std::string ClientSession::AudioError() const {
    const auto last = impl_->voice.LastError();
    if (!last.empty()) return last;
    std::lock_guard lock(impl_->audioErrorMutex);
    return impl_->audioError;
}
ConnectionState ClientSession::State() const noexcept { return impl_->network.State(); }
std::uint32_t ClientSession::SessionId() const noexcept { return impl_->network.SessionId(); }

bool ClientSession::CreateChannel(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    protocol::Writer writer;
    if (!writer.String(name, 64)) return false;
    return impl_->network.SendControl(protocol::ControlType::CreateChannel, writer.Data());
}

bool ClientSession::JoinChannel(const std::uint32_t channelId) {
    if (channelId == 0) return false;
    protocol::Writer writer;
    writer.U32(channelId);
    return impl_->network.SendControl(protocol::ControlType::JoinChannel, writer.Data());
}

bool ClientSession::LeaveChannel() {
    return impl_->network.SendControl(protocol::ControlType::LeaveChannel);
}

bool ClientSession::SetMicrophoneMuted(const bool muted) {
    impl_->voice.SetMicrophoneMuted(muted);
    protocol::Writer writer;
    writer.U8(muted ? 1 : 0);
    return impl_->network.SendControl(protocol::ControlType::SetMuted, writer.Data());
}

bool ClientSession::SetSharing(const bool sharing, const std::uint16_t width, const std::uint16_t height) {
    if (sharing && (width == 0 || height == 0)) return false;
    protocol::Writer writer;
    writer.U8(sharing ? 1 : 0);
    writer.U16(width);
    writer.U16(height);
    return impl_->network.SendControl(protocol::ControlType::SetSharing, writer.Data());
}

bool ClientSession::SetSystemAudioEnabled(const bool enabled, std::string& error) {
    if (!impl_->audioRunning.load()) {
        error = "Audio is not available";
        return false;
    }
    return impl_->voice.SetSystemAudioEnabled(enabled, error);
}

bool ClientSession::SystemAudioEnabled() const noexcept { return impl_->voice.IsSystemAudioEnabled(); }

bool ClientSession::SendVideoFrame(const std::span<const std::uint8_t> jpeg, const bool keyframe) {
    if (jpeg.empty() || jpeg.size() > protocol::kMaxVideoFrameBytes) return false;
    Impl::VideoJob job;
    job.jpeg.assign(jpeg.begin(), jpeg.end());
    job.keyframe = keyframe;
    // A keyframe was explicitly requested (or opens a share); make room so the
    // drop-oldest policy cannot throw it away behind live frames.
    if (keyframe) impl_->videoQueue.Clear();
    return impl_->videoQueue.TryPush(std::move(job));
}

void ClientSession::SetVideoBitrate(const double bitrateBps) noexcept {
    impl_->videoBitrateBps.store(bitrateBps > 0.0 ? bitrateBps : kDefaultVideoBitrateBps);
}

bool ClientSession::RequestKeyframe(const std::uint32_t targetSessionId) {
    return impl_->network.RequestKeyframe(targetSessionId);
}

bool ClientSession::SendChat(const std::string& text) { return impl_->network.SendChat(text); }

void ClientSession::SetVideoFrameCallback(VideoFrameCallback callback) {
    std::lock_guard lock(impl_->videoMutex);
    impl_->videoCallback = std::move(callback);
}

void ClientSession::SetUserVolume(const std::uint32_t userId, const float volume) { impl_->voice.SetUserVolume(userId, volume); }
void ClientSession::SetUserMuted(const std::uint32_t userId, const bool muted) { impl_->voice.SetUserMuted(userId, muted); }
void ClientSession::SetUserScreenVolume(const std::uint32_t userId, const float volume) { impl_->voice.SetUserScreenVolume(userId, volume); }
float ClientSession::LocalLevel() const noexcept { return impl_->voice.LocalLevel(); }
float ClientSession::UserLevel(const std::uint32_t userId) const { return impl_->voice.UserLevel(userId); }
float ClientSession::UserScreenLevel(const std::uint32_t userId) const { return impl_->voice.UserScreenLevel(userId); }
std::uint64_t ClientSession::ReceivedPackets() const noexcept { return impl_->voice.ReceivedPackets(); }
std::uint64_t ClientSession::ReceivedSystemPackets() const noexcept { return impl_->voice.ReceivedSystemPackets(); }

std::optional<NetworkEvent> ClientSession::TryPopEvent() { return impl_->network.TryPopEvent(); }
std::vector<NetworkEvent> ClientSession::PopEvents() { return impl_->network.PopEvents(); }

} // namespace vibez::client
