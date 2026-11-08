#include "client/audio/VoiceEngine.hpp"

#include "client/audio/AudioFormat.hpp"
#include "client/audio/AudioMixer.hpp"
#include "client/audio/AudioThread.hpp"
#include "client/audio/BoundedQueue.hpp"
#include "client/audio/JitterBuffer.hpp"
#include "client/audio/OpusCodec.hpp"
#include "client/audio/WasapiCapture.hpp"
#include "client/audio/WasapiPlayback.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace vibez::audio {
namespace {
std::uint64_t NowMilliseconds() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

constexpr auto kDecodeTick = std::chrono::milliseconds(10);
constexpr std::uint64_t kPlayoutTimeoutMs = 600;
constexpr int kSystemAudioBitrate = 64000;
} // namespace

struct VoiceEngine::Impl {
    struct CapturedFrame {
        std::vector<std::int16_t> samples;
        std::uint64_t timestamp{};
        bool systemAudio{};
    };
    struct UserRuntime {
        std::shared_ptr<VoiceDecoder> decoder;
        std::shared_ptr<JitterBuffer> jitter;
        std::atomic<std::uint64_t> lastPacketMs{};
        std::uint64_t lastDecodeMs{};
    };

    AudioConfig config;
    std::unique_ptr<VoiceEncoder> micEncoder;
    std::unique_ptr<VoiceEncoder> systemEncoder;
    WasapiCapture capture;
    WasapiCapture loopback;
    WasapiPlayback playback;
    AudioMixer mixer{96000};
    BlockingQueue<CapturedFrame> encodeQueue{32};
    std::atomic_bool running{};
    std::atomic_bool stopping{};
    std::atomic_bool microphoneMuted{};
    std::atomic_bool systemAudioEnabled{};
    std::atomic<float> localLevel{};
    std::atomic<std::uint64_t> receivedPackets{};
    std::atomic<std::uint64_t> receivedSystemPackets{};
    std::atomic<std::uint32_t> micSequence{};
    std::atomic<std::uint32_t> systemSequence{};
    std::mutex sendMutex;
    SendCallback send;
    std::thread encodeThread;
    std::thread decodeThread;
    std::mutex usersMutex;
    std::unordered_map<std::uint64_t, std::shared_ptr<UserRuntime>> users;
    mutable std::mutex errorMutex;
    std::string lastError;

    static std::uint64_t Key(const std::uint32_t userId, const bool systemAudio) {
        return (static_cast<std::uint64_t>(userId) << 1) | (systemAudio ? 1U : 0U);
    }

    void SetError(std::string text) {
        std::lock_guard lock(errorMutex);
        lastError = std::move(text);
    }

    void QueueOutgoing(const std::span<const std::int16_t> frame, const std::uint64_t timestamp,
                       const bool systemAudio) {
        if (stopping.load()) return;
        if (systemAudio) {
            if (!systemAudioEnabled.load()) return;
        } else {
            if (microphoneMuted.load()) {
                localLevel.store(0.0F);
                return;
            }
            localLevel.store(RmsLevel(frame));
        }
        CapturedFrame captured;
        captured.samples.assign(frame.begin(), frame.end());
        captured.timestamp = timestamp;
        captured.systemAudio = systemAudio;
        (void)encodeQueue.TryPush(std::move(captured));
    }

    void CloseWorkers() {
        stopping.store(true);
        capture.Stop();
        loopback.Stop();
        playback.Stop();
        encodeQueue.Close();
        if (encodeThread.joinable()) encodeThread.join();
        if (decodeThread.joinable()) decodeThread.join();
        running.store(false);
        localLevel.store(0.0F);
        encodeQueue.Clear();
        {
            std::lock_guard lock(usersMutex);
            users.clear();
        }
        mixer.Reset();
        systemAudioEnabled.store(false);
    }

    void StartEncodeThread() {
        encodeThread = std::thread([this] {
            AudioThreadScope audioThread;
            while (!stopping.load()) {
                CapturedFrame frame;
                if (!encodeQueue.WaitPop(frame, std::chrono::milliseconds(100))) continue;
                if (stopping.load()) break;
                EncodedVoiceFrame encoded;
                encoded.timestamp = frame.timestamp;
                encoded.systemAudio = frame.systemAudio;
                try {
                    if (frame.systemAudio) {
                        encoded.sequence = systemSequence.fetch_add(1);
                        encoded.payload = systemEncoder->Encode(frame.samples);
                    } else {
                        encoded.sequence = micSequence.fetch_add(1);
                        encoded.payload = micEncoder->Encode(frame.samples);
                    }
                } catch (...) {
                    SetError("Falha ao codificar áudio (Opus).");
                    continue;
                }
                SendCallback callback;
                {
                    std::lock_guard lock(sendMutex);
                    callback = send;
                }
                if (callback && !encoded.payload.empty()) callback(encoded);
            }
        });
    }

    void StartDecodeThread() {
        decodeThread = std::thread([this] {
            AudioThreadScope audioThread;
            while (!stopping.load()) {
                const auto tickStart = NowMilliseconds();
                std::vector<std::pair<std::uint64_t, std::shared_ptr<UserRuntime>>> active;
                {
                    std::lock_guard lock(usersMutex);
                    active.reserve(users.size());
                    for (auto& [key, user] : users) active.emplace_back(key, user);
                }
                for (const auto& [key, user] : active) {
                    if (stopping.load()) break;
                    const auto now = NowMilliseconds();
                    if (now - user->lastDecodeMs < 20) continue;
                    user->lastDecodeMs = now;
                    const auto packet = user->jitter->PopNext();
                    const bool recent = now - user->lastPacketMs.load() < kPlayoutTimeoutMs;
                    if (!packet && (!recent || !user->jitter->Primed())) continue;
                    try {
                        const auto pcm = user->decoder->Decode(packet ? std::span<const std::uint8_t>(*packet) : std::span<const std::uint8_t>{});
                        if (!pcm.empty()) mixer.PushSamples(static_cast<std::uint32_t>(key >> 1), (key & 1U) != 0, pcm);
                    } catch (...) {
                        continue;
                    }
                }
                const auto elapsed = NowMilliseconds() - tickStart;
                const auto remaining = static_cast<std::uint64_t>(kDecodeTick.count());
                if (elapsed < remaining) std::this_thread::sleep_for(std::chrono::milliseconds(remaining - elapsed));
            }
        });
    }
};

VoiceEngine::VoiceEngine() : impl_(std::make_unique<Impl>()) {}
VoiceEngine::~VoiceEngine() { impl_->CloseWorkers(); }

bool VoiceEngine::Start(const AudioConfig& config, SendCallback send, std::string& error) {
    EnableAudioProcessPowerThrottling();
    impl_->CloseWorkers();
    impl_->config = config;
    impl_->microphoneMuted.store(config.microphoneMuted);
    impl_->systemAudioEnabled.store(false);
    impl_->mixer.SetMasterVolume(config.masterVolume);
    impl_->micSequence.store(0);
    impl_->systemSequence.store(0);
    impl_->receivedPackets.store(0);
    impl_->receivedSystemPackets.store(0);
    impl_->SetError({});
    {
        std::lock_guard lock(impl_->sendMutex);
        impl_->send = std::move(send);
    }
    try {
        impl_->micEncoder = std::make_unique<VoiceEncoder>(24000, false);
        impl_->systemEncoder = std::make_unique<VoiceEncoder>(kSystemAudioBitrate, true);
    } catch (const std::exception& exception) {
        error = std::string("Opus encoder initialization failed: ") + exception.what();
        impl_->SetError(error);
        return false;
    }
    impl_->stopping.store(false);
    impl_->running.store(true);
    impl_->encodeQueue.Reopen();
    impl_->encodeQueue.Clear();
    impl_->StartEncodeThread();
    impl_->StartDecodeThread();

    const auto playbackError = [&](const std::string& message) {
        error = message;
        impl_->SetError(message);
        impl_->CloseWorkers();
        return false;
    };

    if (!impl_->playback.Start(config.playbackDeviceId, [this](const std::span<std::int16_t> samples) {
            impl_->mixer.Mix(samples);
        }, error)) {
        return playbackError(error);
    }

    if (!impl_->capture.Start(config.captureDeviceId, [this](const std::span<const std::int16_t> frame, const std::uint64_t timestamp) {
            impl_->QueueOutgoing(frame, timestamp, false);
        }, error)) {
        return playbackError(error);
    }

    if (config.systemAudioEnabled && !SetSystemAudioEnabled(true, error)) return playbackError(error);

    error.clear();
    return true;
}

bool VoiceEngine::Restart(std::string& error) {
    SendCallback callback;
    {
        std::lock_guard lock(impl_->sendMutex);
        callback = impl_->send;
    }
    AudioConfig config = impl_->config;
    config.microphoneMuted = impl_->microphoneMuted.load();
    const bool systemAudio = impl_->systemAudioEnabled.load();
    Stop();
    if (!Start(config, std::move(callback), error)) return false;
    if (systemAudio && !SetSystemAudioEnabled(true, error)) return false;
    error.clear();
    return true;
}

void VoiceEngine::Stop() { impl_->CloseWorkers(); }
bool VoiceEngine::IsRunning() const noexcept { return impl_->running.load(); }
void VoiceEngine::SetMicrophoneMuted(const bool muted) { impl_->microphoneMuted.store(muted); if (muted) impl_->localLevel.store(0.0F); }
bool VoiceEngine::IsMicrophoneMuted() const noexcept { return impl_->microphoneMuted.load(); }

bool VoiceEngine::SetSystemAudioEnabled(const bool enabled, std::string& error) {
    if (enabled == impl_->systemAudioEnabled.load()) {
        error.clear();
        return true;
    }
    if (!enabled) {
        impl_->loopback.Stop();
        impl_->systemAudioEnabled.store(false);
        error.clear();
        return true;
    }
    if (!impl_->running.load()) {
        error = "Audio engine is not running";
        return false;
    }
    if (!impl_->loopback.Start(L"", [this](const std::span<const std::int16_t> frame, const std::uint64_t timestamp) {
            impl_->QueueOutgoing(frame, timestamp, true);
        }, error, CaptureSource::SystemLoopback)) {
        return false;
    }
    impl_->systemAudioEnabled.store(true);
    error.clear();
    return true;
}

bool VoiceEngine::IsSystemAudioEnabled() const noexcept { return impl_->systemAudioEnabled.load(); }
void VoiceEngine::SetUserVolume(const std::uint32_t userId, const float volume) { impl_->mixer.SetUserVolume(userId, volume); }
void VoiceEngine::SetUserMuted(const std::uint32_t userId, const bool muted) { impl_->mixer.SetUserMuted(userId, muted); }
void VoiceEngine::SetUserScreenVolume(const std::uint32_t userId, const float volume) {
    impl_->mixer.SetUserVolume(userId, true, volume);
}

void VoiceEngine::RemoveUser(const std::uint32_t userId) {
    {
        std::lock_guard lock(impl_->usersMutex);
        impl_->users.erase(Impl::Key(userId, false));
        impl_->users.erase(Impl::Key(userId, true));
    }
    impl_->mixer.RemoveUser(userId);
}

void VoiceEngine::SubmitPacket(const std::uint32_t userId, const bool systemAudio, const std::uint32_t sequence,
                               const std::uint64_t timestamp, const std::span<const std::uint8_t> payload) {
    if (payload.empty() || impl_->stopping.load()) return;
    const auto key = Impl::Key(userId, systemAudio);
    std::shared_ptr<Impl::UserRuntime> user;
    {
        std::lock_guard lock(impl_->usersMutex);
        auto [it, inserted] = impl_->users.try_emplace(key);
        if (inserted || !it->second) {
            user = std::make_shared<Impl::UserRuntime>();
            try {
                user->decoder = std::make_shared<VoiceDecoder>();
            } catch (...) {
                impl_->users.erase(it);
                return;
            }
            user->jitter = std::make_shared<JitterBuffer>(16);
            it->second = user;
        } else {
            user = it->second;
        }
    }
    (void)timestamp;
    user->lastPacketMs.store(NowMilliseconds());
    if (user->jitter->Push(sequence, payload)) {
        impl_->receivedPackets.fetch_add(1, std::memory_order_relaxed);
        if (systemAudio) impl_->receivedSystemPackets.fetch_add(1, std::memory_order_relaxed);
    }
}

float VoiceEngine::LocalLevel() const noexcept { return impl_->localLevel.load(); }
float VoiceEngine::UserLevel(const std::uint32_t userId) const { return impl_->mixer.PeakLevel(userId); }
float VoiceEngine::UserScreenLevel(const std::uint32_t userId) const { return impl_->mixer.PeakLevel(userId, true); }
std::uint64_t VoiceEngine::ReceivedPackets() const noexcept { return impl_->receivedPackets.load(); }
std::uint64_t VoiceEngine::ReceivedSystemPackets() const noexcept { return impl_->receivedSystemPackets.load(); }

std::string VoiceEngine::LastError() const {
    std::lock_guard lock(impl_->errorMutex);
    return impl_->lastError;
}

} // namespace vibez::audio
