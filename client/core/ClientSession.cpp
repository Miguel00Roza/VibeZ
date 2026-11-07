#include "client/core/ClientSession.hpp"

#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <utility>

namespace vibez::client {
namespace {
void SetAudioError(std::mutex& mutex, std::string& target, std::string text) {
    std::lock_guard lock(mutex);
    target = std::move(text);
}
} // namespace

struct ClientSession::Impl {
    struct PendingVideoFrame {
        std::uint32_t frameId{};
        std::uint16_t fragmentCount{};
        bool keyframe{};
        std::size_t totalBytes{};
        std::vector<std::vector<std::uint8_t>> fragments;
        std::vector<bool> received;
        std::uint16_t receivedCount{};
    };

    ClientSessionConfig config;
    NetworkClient network;
    audio::VoiceEngine voice;
    std::atomic_bool audioRunning{};
    mutable std::mutex audioErrorMutex;
    std::string audioError;
    std::atomic_uint32_t nextVideoSequence{1};
    std::atomic_uint32_t nextVideoFrameId{1};
    std::mutex videoMutex;
    std::unordered_map<std::uint32_t, PendingVideoFrame> pendingVideo;
    VideoFrameCallback videoCallback;

    void SubmitVideo(const MediaPacket& packet) {
        if (packet.type != protocol::MediaType::Video || packet.fragmentCount == 0 ||
            packet.fragmentCount > protocol::kMaxVideoFragments || packet.fragmentIndex >= packet.fragmentCount ||
            packet.payload.empty()) return;

        VideoFrame complete;
        VideoFrameCallback callback;
        {
            std::lock_guard lock(videoMutex);
            auto& pending = pendingVideo[packet.sessionId];
            if (pending.frameId != packet.frameId || pending.fragmentCount != packet.fragmentCount) {
                pending = {};
                pending.frameId = packet.frameId;
                pending.fragmentCount = packet.fragmentCount;
                pending.keyframe = packet.flags & protocol::kMediaFlagKeyframe;
                pending.fragments.resize(packet.fragmentCount);
                pending.received.assign(packet.fragmentCount, false);
            }
            if (!pending.received[packet.fragmentIndex]) {
                pending.totalBytes += packet.payload.size();
                if (pending.totalBytes > protocol::kMaxVideoFrameBytes) {
                    pendingVideo.erase(packet.sessionId);
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
            pendingVideo.erase(packet.sessionId);
            callback = videoCallback;
        }
        if (callback) callback(complete);
    }
};

ClientSession::ClientSession() : impl_(std::make_unique<Impl>()) {}

ClientSession::~ClientSession() { impl_->network.Stop(); impl_->voice.Stop(); }

bool ClientSession::Start(const ClientSessionConfig& config, std::string& error) {
    impl_->network.Stop();
    impl_->voice.Stop();
    impl_->audioRunning.store(false);
    impl_->config = config;

    std::string networkError;
    if (!impl_->network.Start(config.network, networkError)) {
        error = networkError;
        return false;
    }

    impl_->network.SetAudioSink([this](const MediaPacket& packet) {
        if (packet.type != protocol::MediaType::Audio || !impl_->audioRunning.load()) return;
        impl_->voice.SubmitPacket(packet.sessionId, packet.sequence, packet.timestamp, packet.payload);
    });
    impl_->network.SetVideoSink([this](const MediaPacket& packet) { impl_->SubmitVideo(packet); });

    if (!config.enableAudio) {
        SetAudioError(impl_->audioErrorMutex, impl_->audioError, "Audio disabled by configuration");
    } else {
        std::string audioError;
        if (impl_->voice.Start(config.audio, [this](const audio::EncodedVoiceFrame& frame) {
                (void)impl_->network.SendAudio(frame.sequence, frame.timestamp, frame.payload);
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
    impl_->network.Stop();
    impl_->voice.Stop();
}

bool ClientSession::IsRunning() const noexcept { return impl_->network.IsRunning(); }
bool ClientSession::AudioRunning() const noexcept { return impl_->audioRunning.load(); }
std::string ClientSession::AudioError() const {
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
    constexpr std::size_t kVideoPayloadBytes = 1100; // keeps UDP datagrams below a typical MTU.
    if (jpeg.empty() || jpeg.size() > protocol::kMaxVideoFrameBytes) return false;
    const auto fragmentCount = (jpeg.size() + kVideoPayloadBytes - 1) / kVideoPayloadBytes;
    if (fragmentCount == 0 || fragmentCount > protocol::kMaxVideoFragments) return false;
    const auto frameId = impl_->nextVideoFrameId.fetch_add(1);
    const auto timestamp = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    const auto flags = static_cast<std::uint8_t>(keyframe ? protocol::kMediaFlagKeyframe : 0);
    for (std::size_t index = 0; index < fragmentCount; ++index) {
        const auto offset = index * kVideoPayloadBytes;
        const auto bytes = std::min(kVideoPayloadBytes, jpeg.size() - offset);
        if (!impl_->network.SendVideo(impl_->nextVideoSequence.fetch_add(1), timestamp, flags, frameId,
                                      static_cast<std::uint16_t>(index), static_cast<std::uint16_t>(fragmentCount),
                                      jpeg.subspan(offset, bytes))) return false;
    }
    return true;
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
float ClientSession::LocalLevel() const noexcept { return impl_->voice.LocalLevel(); }
float ClientSession::UserLevel(const std::uint32_t userId) const { return impl_->voice.UserLevel(userId); }
std::uint64_t ClientSession::ReceivedPackets() const noexcept { return impl_->voice.ReceivedPackets(); }

std::optional<NetworkEvent> ClientSession::TryPopEvent() { return impl_->network.TryPopEvent(); }
std::vector<NetworkEvent> ClientSession::PopEvents() { return impl_->network.PopEvents(); }

} // namespace vibez::client
