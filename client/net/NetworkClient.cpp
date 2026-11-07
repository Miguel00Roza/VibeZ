#include "client/net/NetworkClient.hpp"

#include "client/net/EventQueue.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <utility>

namespace vibez::client {
namespace {
using protocol::ControlMessage;
using protocol::ControlType;
using protocol::MediaType;

constexpr int kConnectTimeoutSeconds = 3;
constexpr int kInitialBackoffMs = 500;
constexpr int kMaxBackoffMs = 5000;
constexpr std::size_t kMaxPresenceUsers = 4096;
constexpr std::size_t kMaxChannels = 1024;

NetworkEvent MakeStateEvent(const ConnectionState value) {
    NetworkEvent event;
    event.type = NetworkEvent::Type::StateChanged;
    event.state = value;
    return event;
}

bool ResolveServer(const std::string& host, const std::uint16_t port, sockaddr_in& address) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* result = nullptr;
    const auto portText = std::to_string(port);
    if (getaddrinfo(host.c_str(), portText.c_str(), &hints, &result) != 0) return false;
    bool resolved = false;
    for (const auto* entry = result; entry != nullptr; entry = entry->ai_next) {
        if (entry->ai_family != AF_INET || entry->ai_addr == nullptr) continue;
        address = *reinterpret_cast<const sockaddr_in*>(entry->ai_addr);
        resolved = true;
        break;
    }
    freeaddrinfo(result);
    return resolved;
}

std::string SocketErrorText(const char* operation) {
    return std::string(operation) + " failed (Winsock error " + std::to_string(WSAGetLastError()) + ")";
}
} // namespace

struct NetworkClient::Impl {
    NetworkConfig config;
    std::thread thread;
    std::atomic_bool running{};
    std::atomic_bool stopping{};
    std::atomic_bool wsaStarted{};
    std::atomic<ConnectionState> state{ConnectionState::Disconnected};
    std::atomic<std::uint32_t> sessionId{};
    std::atomic<std::uint32_t> nextRequestId{1};

    mutable std::mutex socketMutex;
    SOCKET tcpSocket{INVALID_SOCKET};
    SOCKET udpSocket{INVALID_SOCKET};
    sockaddr_in serverAddress{};

    EventQueue<NetworkEvent> events{512};

    mutable std::mutex sinkMutex;
    MediaSink audioSink;
    MediaSink videoSink;

    ~Impl() { Stop(); }

    void EmitState(const ConnectionState value) {
        state.store(value);
        events.Push(MakeStateEvent(value));
    }

    void EmitError(const std::string& text, const std::uint32_t requestId = 0) {
        NetworkEvent event;
        event.type = NetworkEvent::Type::Error;
        event.requestId = requestId;
        event.text = text;
        events.Push(std::move(event));
    }

    void SleepWhileRunning(const int milliseconds) {
        for (int waited = 0; waited < milliseconds && !stopping.load(); waited += 50) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    void CloseSockets() {
        std::lock_guard lock(socketMutex);
        if (tcpSocket != INVALID_SOCKET) {
            closesocket(tcpSocket);
            tcpSocket = INVALID_SOCKET;
        }
        if (udpSocket != INVALID_SOCKET) {
            closesocket(udpSocket);
            udpSocket = INVALID_SOCKET;
        }
    }

    bool SendTcp(const std::vector<std::uint8_t>& bytes) {
        if (bytes.empty()) return false;
        std::lock_guard lock(socketMutex);
        if (tcpSocket == INVALID_SOCKET) return false;
        std::size_t sent = 0;
        while (sent < bytes.size()) {
            const auto remaining = static_cast<int>(bytes.size() - sent);
            const int result = send(tcpSocket, reinterpret_cast<const char*>(bytes.data() + sent), remaining, 0);
            if (result > 0) {
                sent += static_cast<std::size_t>(result);
                continue;
            }
            if (WSAGetLastError() != WSAEWOULDBLOCK) return false;
            fd_set writable;
            FD_ZERO(&writable);
            FD_SET(tcpSocket, &writable);
            timeval timeout{1, 0};
            if (select(0, nullptr, &writable, nullptr, &timeout) <= 0) return false;
        }
        return true;
    }

    bool SendUdp(const std::vector<std::uint8_t>& bytes) {
        if (bytes.empty()) return false;
        std::lock_guard lock(socketMutex);
        if (udpSocket == INVALID_SOCKET) return false;
        return sendto(udpSocket, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
                      reinterpret_cast<const sockaddr*>(&serverAddress), sizeof(serverAddress)) == static_cast<int>(bytes.size());
    }

    bool SendControlInternal(const ControlType type, const std::span<const std::uint8_t> payload) {
        if (payload.size() > protocol::kMaxControlPayload) return false;
        const auto requestId = nextRequestId.fetch_add(1);
        const ControlMessage message{type, requestId, std::vector<std::uint8_t>(payload.begin(), payload.end())};
        return SendTcp(protocol::EncodeControl(message));
    }

    bool CreateSockets() {
        sockaddr_in server{};
        if (!ResolveServer(config.host, config.port, server)) {
            if (!config.autoReconnect) EmitError("Could not resolve server address");
            return false;
        }

        const SOCKET udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (udp == INVALID_SOCKET) {
            if (!config.autoReconnect) EmitError(SocketErrorText("socket(UDP)"));
            return false;
        }
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        int localSize = sizeof(local);
        if (bind(udp, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR ||
            getsockname(udp, reinterpret_cast<sockaddr*>(&local), &localSize) == SOCKET_ERROR) {
            if (!config.autoReconnect) EmitError(SocketErrorText("bind(UDP)"));
            closesocket(udp);
            return false;
        }
        const auto udpPort = ntohs(local.sin_port);

        const SOCKET tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (tcp == INVALID_SOCKET) {
            if (!config.autoReconnect) EmitError(SocketErrorText("socket(TCP)"));
            closesocket(udp);
            return false;
        }
        u_long nonBlocking = 1;
        if (ioctlsocket(tcp, FIONBIO, &nonBlocking) == SOCKET_ERROR) {
            if (!config.autoReconnect) EmitError(SocketErrorText("ioctlsocket"));
            closesocket(udp);
            closesocket(tcp);
            return false;
        }
        int noDelay = 1;
        setsockopt(tcp, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));

        bool connected = connect(tcp, reinterpret_cast<sockaddr*>(&server), sizeof(server)) != SOCKET_ERROR;
        if (!connected) {
            if (WSAGetLastError() != WSAEWOULDBLOCK) {
                if (!config.autoReconnect) EmitError(SocketErrorText("connect"));
                closesocket(udp);
                closesocket(tcp);
                return false;
            }
            fd_set writable;
            fd_set failed;
            FD_ZERO(&writable);
            FD_ZERO(&failed);
            FD_SET(tcp, &writable);
            FD_SET(tcp, &failed);
            timeval timeout{kConnectTimeoutSeconds, 0};
            const int selected = select(0, nullptr, &writable, &failed, &timeout);
            if (selected <= 0 || FD_ISSET(tcp, &failed)) {
                if (!config.autoReconnect) EmitError("Timed out while connecting to the server");
                closesocket(udp);
                closesocket(tcp);
                return false;
            }
            int error = 0;
            int errorSize = sizeof(error);
            if (getsockopt(tcp, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &errorSize) == SOCKET_ERROR || error != 0) {
                if (!config.autoReconnect) EmitError("Could not connect to the server");
                closesocket(udp);
                closesocket(tcp);
                return false;
            }
        }

        {
            std::lock_guard lock(socketMutex);
            serverAddress = server;
            tcpSocket = tcp;
            udpSocket = udp;
        }

        protocol::Writer hello;
        if (!hello.String(config.nickname, 32)) {
            CloseSockets();
            if (!config.autoReconnect) EmitError("Nickname is invalid");
            return false;
        }
        hello.U16(udpPort);
        if (!hello.String(config.token, 128)) {
            CloseSockets();
            if (!config.autoReconnect) EmitError("Token is invalid");
            return false;
        }
        if (!SendControlInternal(ControlType::Hello, hello.Data())) {
            CloseSockets();
            if (!config.autoReconnect) EmitError("Could not send Hello");
            return false;
        }
        return true;
    }

    void HandleHelloAccepted(const ControlMessage& message) {
        protocol::Reader reader(message.payload);
        NetworkEvent event;
        event.type = NetworkEvent::Type::HelloAccepted;
        event.requestId = message.requestId;
        if (!reader.U32(event.sessionId) || !reader.U32(event.channelId) || event.sessionId == 0) {
            EmitError("Malformed HelloAccepted message", message.requestId);
            return;
        }
        sessionId.store(event.sessionId);
        events.Push(std::move(event));
    }

    void HandleError(const ControlMessage& message) {
        protocol::Reader reader(message.payload);
        std::string text;
        if (!reader.String(text, 200)) text = "Unreadable server error";
        EmitError(text, message.requestId);
    }

    void HandleChannels(const ControlMessage& message) {
        protocol::Reader reader(message.payload);
        std::uint32_t count{};
        if (!reader.U32(count) || count > kMaxChannels) {
            EmitError("Malformed channel snapshot", message.requestId);
            return;
        }
        NetworkEvent event;
        event.type = NetworkEvent::Type::ChannelsUpdated;
        event.requestId = message.requestId;
        event.channels.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            ChannelInfo channel;
            if (!reader.U32(channel.id) || !reader.String(channel.name, 64)) {
                EmitError("Malformed channel entry", message.requestId);
                return;
            }
            event.channels.push_back(std::move(channel));
        }
        events.Push(std::move(event));
    }

    void HandlePresence(const ControlMessage& message) {
        protocol::Reader reader(message.payload);
        std::uint32_t count{};
        if (!reader.U32(count) || count > kMaxPresenceUsers) {
            EmitError("Malformed presence snapshot", message.requestId);
            return;
        }
        NetworkEvent event;
        event.type = NetworkEvent::Type::PresenceUpdated;
        event.requestId = message.requestId;
        event.users.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            UserInfo user;
            std::uint8_t muted{};
            std::uint8_t sharing{};
            if (!reader.U32(user.id) || !reader.U32(user.channel) || !reader.U8(muted) || !reader.U8(sharing) ||
                !reader.U16(user.shareWidth) || !reader.U16(user.shareHeight) || !reader.String(user.name, 32)) {
                EmitError("Malformed presence entry", message.requestId);
                return;
            }
            user.muted = muted != 0;
            user.sharing = sharing != 0;
            event.users.push_back(std::move(user));
        }
        events.Push(std::move(event));
    }

    void HandleKeyframeRequest(const ControlMessage& message) {
        protocol::Reader reader(message.payload);
        NetworkEvent event;
        event.type = NetworkEvent::Type::KeyframeRequest;
        event.requestId = message.requestId;
        if (!reader.U32(event.sessionId)) {
            EmitError("Malformed keyframe request", message.requestId);
            return;
        }
        events.Push(std::move(event));
    }

    void HandleChatMessage(const ControlMessage& message) {
        protocol::Reader reader(message.payload);
        NetworkEvent event;
        event.type = NetworkEvent::Type::ChatMessage;
        event.requestId = message.requestId;
        if (!reader.U32(event.sessionId) || !reader.String(event.senderName, 32) ||
            !reader.String(event.text, 1000) || reader.Remaining() != 0) {
            EmitError("Malformed chat message", message.requestId);
            return;
        }
        events.Push(std::move(event));
    }

    void HandleControl(const ControlMessage& message) {
        switch (message.type) {
        case ControlType::HelloAccepted: HandleHelloAccepted(message); break;
        case ControlType::Error: HandleError(message); break;
        case ControlType::ChannelSnapshot: HandleChannels(message); break;
        case ControlType::PresenceSnapshot: HandlePresence(message); break;
        case ControlType::RequestKeyframe: HandleKeyframeRequest(message); break;
        case ControlType::ChatMessage: HandleChatMessage(message); break;
        default: break;
        }
    }

    void HandleMedia(const SOCKET udp) {
        std::array<std::uint8_t, protocol::kMaxMediaPayload + 32> buffer{};
        sockaddr_in sender{};
        int senderSize = sizeof(sender);
        const int received = recvfrom(udp, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0,
                                      reinterpret_cast<sockaddr*>(&sender), &senderSize);
        if (received <= 0) return;
        const auto header = protocol::DecodeMediaHeader(
            std::span<const std::uint8_t>(buffer.data(), static_cast<std::size_t>(received)));
        if (!header) return;
        const auto ownId = sessionId.load();
        if (ownId != 0 && header->sessionId == ownId) return;

        MediaPacket packet;
        packet.type = header->type;
        packet.sessionId = header->sessionId;
        packet.sequence = header->sequence;
        packet.timestamp = header->timestamp;
        packet.flags = header->flags;
        packet.frameId = header->frameId;
        packet.fragmentIndex = header->fragmentIndex;
        packet.fragmentCount = header->fragmentCount;
        packet.payload.assign(
            buffer.begin() + static_cast<std::ptrdiff_t>(protocol::kMediaHeaderSize),
            buffer.begin() + static_cast<std::ptrdiff_t>(received));

        MediaSink sink;
        {
            std::lock_guard lock(sinkMutex);
            sink = packet.type == MediaType::Audio ? audioSink : videoSink;
        }
        if (sink) sink(packet);
    }

    // Returns false when the connection ended unexpectedly (so Run can emit a
    // diagnostic); stopping is handled without an error.
    bool ReceiveLoop() {
        std::vector<std::uint8_t> stream;
        while (!stopping.load()) {
            SOCKET tcp = INVALID_SOCKET;
            SOCKET udp = INVALID_SOCKET;
            {
                std::lock_guard lock(socketMutex);
                tcp = tcpSocket;
                udp = udpSocket;
            }
            if (tcp == INVALID_SOCKET || udp == INVALID_SOCKET) return false;

            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(tcp, &readable);
            FD_SET(udp, &readable);
            timeval timeout{0, 100000};
            const int selected = select(0, &readable, nullptr, nullptr, &timeout);
            if (selected == SOCKET_ERROR) return false;
            if (selected == 0) continue;

            if (FD_ISSET(udp, &readable)) HandleMedia(udp);
            if (FD_ISSET(tcp, &readable)) {
                std::array<std::uint8_t, 8192> input{};
                const int received = recv(tcp, reinterpret_cast<char*>(input.data()), static_cast<int>(input.size()), 0);
                if (received <= 0) return false;
                stream.insert(stream.end(), input.begin(), input.begin() + received);
                while (const auto message = protocol::TryReadControl(stream)) HandleControl(*message);
            }
        }
        return false;
    }

    void Run() {
        int backoff = kInitialBackoffMs;
        while (!stopping.load()) {
            if (!CreateSockets()) {
                if (!config.autoReconnect) {
                    EmitState(ConnectionState::Disconnected);
                    break;
                }
                EmitState(ConnectionState::Reconnecting);
                SleepWhileRunning(backoff);
                backoff = (std::min)(backoff * 2, kMaxBackoffMs);
                continue;
            }
            backoff = kInitialBackoffMs;
            EmitState(ConnectionState::Connected);
            const bool disconnectedUnexpectedly = ReceiveLoop();
            CloseSockets();
            sessionId.store(0);
            if (stopping.load()) break;
            if (disconnectedUnexpectedly) EmitError("Connection to the server was lost");
            if (!config.autoReconnect) {
                EmitState(ConnectionState::Disconnected);
                break;
            }
            EmitState(ConnectionState::Reconnecting);
            SleepWhileRunning(backoff);
            backoff = (std::min)(backoff * 2, kMaxBackoffMs);
        }
        CloseSockets();
        running.store(false);
    }

    bool Start(const NetworkConfig& cfg, std::string& error) {
        if (running.load()) {
            error = "Network client is already running";
            return false;
        }
        if (cfg.host.empty()) {
            error = "Server host must not be empty";
            return false;
        }
        if (cfg.port == 0) {
            error = "Server port must not be zero";
            return false;
        }
        if (cfg.nickname.empty() || cfg.nickname.size() > 32) {
            error = "Nickname must contain 1 to 32 bytes";
            return false;
        }
        if (cfg.token.size() > 128) {
            error = "Token must contain at most 128 bytes";
            return false;
        }
        if (thread.joinable()) thread.join();

        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            error = "WSAStartup failed";
            return false;
        }
        wsaStarted.store(true);
        config = cfg;
        stopping.store(false);
        sessionId.store(0);
        nextRequestId.store(1);
        events.Clear();
        EmitState(ConnectionState::Connecting);
        running.store(true);
        thread = std::thread([this] { Run(); });
        error.clear();
        return true;
    }

    void Stop() {
        running.store(false);
        stopping.store(true);
        if (thread.joinable()) thread.join();
        CloseSockets();
        sessionId.store(0);
        stopping.store(false);
        if (wsaStarted.exchange(false)) WSACleanup();
        if (state.exchange(ConnectionState::Disconnected) != ConnectionState::Disconnected) {
            events.Push(MakeStateEvent(ConnectionState::Disconnected));
        }
    }
};

NetworkClient::NetworkClient() : impl_(std::make_unique<Impl>()) {}
NetworkClient::~NetworkClient() = default;

bool NetworkClient::Start(const NetworkConfig& config, std::string& error) { return impl_->Start(config, error); }
void NetworkClient::Stop() { impl_->Stop(); }
bool NetworkClient::IsRunning() const noexcept { return impl_->running.load(); }
ConnectionState NetworkClient::State() const noexcept { return impl_->state.load(); }
std::uint32_t NetworkClient::SessionId() const noexcept { return impl_->sessionId.load(); }

bool NetworkClient::SendControl(const ControlType type, const std::span<const std::uint8_t> payload) {
    return impl_->SendControlInternal(type, payload);
}

bool NetworkClient::SendAudio(const std::uint32_t sequence, const std::uint64_t timestamp, const std::span<const std::uint8_t> payload) {
    if (!impl_->running.load() || payload.empty() || payload.size() > protocol::kMaxMediaPayload) return false;
    const auto ownId = impl_->sessionId.load();
    if (ownId == 0) return false;
    const protocol::MediaHeader header{MediaType::Audio, ownId, sequence, timestamp, static_cast<std::uint16_t>(payload.size())};
    const auto headerBytes = protocol::EncodeMediaHeader(header);
    if (headerBytes.empty()) return false;
    std::vector<std::uint8_t> packet = headerBytes;
    packet.insert(packet.end(), payload.begin(), payload.end());
    return impl_->SendUdp(packet);
}

bool NetworkClient::SendVideo(const std::uint32_t sequence, const std::uint64_t timestamp, const std::uint8_t flags,
                              const std::uint32_t frameId, const std::uint16_t fragmentIndex,
                              const std::uint16_t fragmentCount, const std::span<const std::uint8_t> payload) {
    if (!impl_->running.load() || payload.size() > protocol::kMaxMediaPayload) return false;
    const auto ownId = impl_->sessionId.load();
    if (ownId == 0) return false;
    const protocol::MediaHeader header{MediaType::Video, ownId, sequence, timestamp,
                                       static_cast<std::uint16_t>(payload.size()), flags, frameId,
                                       fragmentIndex, fragmentCount};
    const auto headerBytes = protocol::EncodeMediaHeader(header);
    if (headerBytes.empty()) return false;
    std::vector<std::uint8_t> packet = headerBytes;
    packet.insert(packet.end(), payload.begin(), payload.end());
    return impl_->SendUdp(packet);
}

bool NetworkClient::RequestKeyframe(const std::uint32_t targetSessionId) {
    protocol::Writer writer;
    writer.U32(targetSessionId);
    return SendControl(ControlType::RequestKeyframe, writer.Data());
}

bool NetworkClient::SendChat(const std::string_view text) {
    if (text.empty() || text.size() > 1000) return false;
    protocol::Writer writer;
    if (!writer.String(std::string(text), 1000)) return false;
    return SendControl(ControlType::ChatMessage, writer.Data());
}

void NetworkClient::SetAudioSink(MediaSink sink) {
    std::lock_guard lock(impl_->sinkMutex);
    impl_->audioSink = std::move(sink);
}

void NetworkClient::SetVideoSink(MediaSink sink) {
    std::lock_guard lock(impl_->sinkMutex);
    impl_->videoSink = std::move(sink);
}

std::optional<NetworkEvent> NetworkClient::TryPopEvent() { return impl_->events.TryPop(); }
std::vector<NetworkEvent> NetworkClient::PopEvents() { return impl_->events.PopAll(); }

} // namespace vibez::client
