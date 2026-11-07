#include "server/include/vibez/server/Server.hpp"
#include "common/protocol/Protocol.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <algorithm>
#include <array>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace vibez::server {
namespace {
using vibez::protocol::ControlMessage;
using vibez::protocol::ControlType;

constexpr std::uint32_t kLobbyChannel = 1;
constexpr std::size_t kMaxChannels = 32;

struct Channel { std::uint32_t id; std::string name; };
struct Session {
    SOCKET socket{INVALID_SOCKET};
    sockaddr_in tcpAddress{};
    sockaddr_in udpAddress{};
    std::uint32_t id{};
    std::uint32_t channel{};
    std::string name;
    bool muted{};
    bool sharing{};
    std::uint16_t shareWidth{};
    std::uint16_t shareHeight{};
    bool authenticated{};
    std::vector<std::uint8_t> received;
};

bool SendAll(SOCKET socket, const std::vector<std::uint8_t>& bytes) {
    std::size_t sent{};
    while (sent < bytes.size()) {
        const auto result = send(socket, reinterpret_cast<const char*>(bytes.data() + sent), static_cast<int>(bytes.size() - sent), 0);
        if (result <= 0) return false;
        sent += static_cast<std::size_t>(result);
    }
    return true;
}

std::vector<std::uint8_t> ErrorPayload(const std::string& text) {
    protocol::Writer writer;
    writer.String(text, 200);
    return writer.Data();
}

std::vector<std::string> LocalIPv4Addresses() {
    ULONG bufferSize{};
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, nullptr, &bufferSize) != ERROR_BUFFER_OVERFLOW) return {};

    std::vector<unsigned char> buffer(bufferSize);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &bufferSize) != NO_ERROR) return {};

    std::vector<std::string> addresses;
    for (auto* adapter = adapters; adapter != nullptr; adapter = adapter->Next) {
        for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
            const auto* address = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
            if (address == nullptr || address->sin_family != AF_INET ||
                address->sin_addr.s_addr == htonl(INADDR_ANY) ||
                address->sin_addr.s_addr == htonl(INADDR_LOOPBACK)) {
                continue;
            }
            char text[INET_ADDRSTRLEN]{};
            if (InetNtopA(AF_INET, &address->sin_addr, text, static_cast<DWORD>(std::size(text))) != nullptr) {
                addresses.emplace_back(text);
            }
        }
    }
    std::sort(addresses.begin(), addresses.end());
    addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());
    return addresses;
}
} // namespace

struct Server::Impl {
    SOCKET tcpListener{INVALID_SOCKET};
    SOCKET udpSocket{INVALID_SOCKET};
    std::unordered_map<SOCKET, Session> sessions;
    std::vector<Channel> channels{{kLobbyChannel, "Lobby"}};
    std::uint32_t nextSessionId{1};
    std::uint32_t nextChannelId{2};

    void CloseSession(const SOCKET socket) {
        const auto it = sessions.find(socket);
        if (it == sessions.end()) return;
        std::cerr << "Client disconnected: " << (it->second.name.empty() ? "pending" : it->second.name) << "\n";
        closesocket(socket);
        sessions.erase(it);
    }

    void Send(const Session& session, const ControlType type, const std::uint32_t requestId, const std::vector<std::uint8_t>& payload) {
        const auto packet = protocol::EncodeControl({type, requestId, payload});
        if (!packet.empty()) SendAll(session.socket, packet);
    }

    void SendError(const Session& session, const std::uint32_t requestId, const std::string& text) {
        Send(session, ControlType::Error, requestId, ErrorPayload(text));
    }

    void Broadcast(const ControlType type, const std::vector<std::uint8_t>& payload) {
        for (const auto& [socket, session] : sessions) {
            if (session.authenticated) Send(session, type, 0, payload);
        }
    }

    void BroadcastChannels() {
        protocol::Writer writer;
        writer.U32(static_cast<std::uint32_t>(channels.size()));
        for (const auto& channel : channels) { writer.U32(channel.id); writer.String(channel.name, 64); }
        Broadcast(ControlType::ChannelSnapshot, writer.Data());
    }

    void BroadcastPresence() {
        protocol::Writer writer;
        const auto count = std::count_if(sessions.begin(), sessions.end(), [](const auto& pair) { return pair.second.authenticated; });
        writer.U32(static_cast<std::uint32_t>(count));
        for (const auto& [socket, session] : sessions) {
            if (!session.authenticated) continue;
            writer.U32(session.id);
            writer.U32(session.channel);
            writer.U8(session.muted ? 1 : 0);
            writer.U8(session.sharing ? 1 : 0);
            writer.U16(session.shareWidth);
            writer.U16(session.shareHeight);
            writer.String(session.name, 32);
        }
        Broadcast(ControlType::PresenceSnapshot, writer.Data());
    }

    void BroadcastChannel(const std::uint32_t channelId, const ControlType type,
                          const std::vector<std::uint8_t>& payload) {
        for (const auto& [socket, session] : sessions) {
            if (session.authenticated && session.channel == channelId) Send(session, type, 0, payload);
        }
    }

    bool ChannelExists(const std::uint32_t id) const {
        return std::any_of(channels.begin(), channels.end(), [id](const Channel& channel) { return channel.id == id; });
    }

    void HandleHello(Session& session, const ControlMessage& message) {
        protocol::Reader reader(message.payload); std::string name; std::uint16_t udpPort{}; std::string token;
        if (!reader.String(name, 32) || name.empty() || !reader.U16(udpPort) || udpPort == 0 || !reader.String(token, 128) || reader.Remaining() != 0) {
            SendError(session, message.requestId, "Invalid hello payload"); return;
        }
        session.name = std::move(name); session.id = nextSessionId++; session.channel = kLobbyChannel; session.authenticated = true;
        session.udpAddress = session.tcpAddress; session.udpAddress.sin_port = htons(udpPort);
        protocol::Writer accepted; accepted.U32(session.id); accepted.U32(kLobbyChannel);
        Send(session, ControlType::HelloAccepted, message.requestId, accepted.Data());
        std::cerr << "Client connected: " << session.name << " (#" << session.id << ")\n";
        BroadcastChannels(); BroadcastPresence();
    }

    void HandleAuthenticated(Session& session, const ControlMessage& message) {
        protocol::Reader reader(message.payload);
        switch (message.type) {
        case ControlType::CreateChannel: {
            std::string name;
            if (!reader.String(name, 64) || name.empty() || reader.Remaining() != 0) { SendError(session, message.requestId, "Invalid channel name"); return; }
            if (channels.size() >= kMaxChannels) { SendError(session, message.requestId, "Channel limit reached"); return; }
            channels.push_back({nextChannelId++, std::move(name)}); BroadcastChannels(); break;
        }
        case ControlType::RemoveChannel: {
            std::uint32_t id{};
            if (!reader.U32(id) || reader.Remaining() != 0 || id == kLobbyChannel) { SendError(session, message.requestId, "Invalid channel"); return; }
            const auto it = std::find_if(channels.begin(), channels.end(), [id](const Channel& c) { return c.id == id; });
            if (it == channels.end()) { SendError(session, message.requestId, "Channel not found"); return; }
            channels.erase(it);
            for (auto& [socket, user] : sessions) if (user.channel == id) { user.channel = kLobbyChannel; user.sharing = false; }
            BroadcastChannels(); BroadcastPresence(); break;
        }
        case ControlType::JoinChannel: {
            std::uint32_t id{};
            if (!reader.U32(id) || reader.Remaining() != 0 || !ChannelExists(id)) { SendError(session, message.requestId, "Channel not found"); return; }
            if (session.channel != id) session.sharing = false;
            session.channel = id; BroadcastPresence(); break;
        }
        case ControlType::LeaveChannel:
            if (reader.Remaining() != 0) { SendError(session, message.requestId, "Invalid leave payload"); return; }
            session.channel = kLobbyChannel; session.sharing = false; BroadcastPresence(); break;
        case ControlType::SetMuted: {
            std::uint8_t muted{};
            if (!reader.U8(muted) || muted > 1 || reader.Remaining() != 0) { SendError(session, message.requestId, "Invalid mute payload"); return; }
            session.muted = muted != 0; BroadcastPresence(); break;
        }
        case ControlType::SetSharing: {
            std::uint8_t sharing{}; std::uint16_t width{}, height{};
            if (!reader.U8(sharing) || sharing > 1 || !reader.U16(width) || !reader.U16(height) || reader.Remaining() != 0) {
                SendError(session, message.requestId, "Invalid sharing payload"); return;
            }
            if (sharing && (width == 0 || height == 0)) { SendError(session, message.requestId, "Invalid sharing size"); return; }
            session.sharing = sharing != 0;
            session.shareWidth = session.sharing ? width : 0;
            session.shareHeight = session.sharing ? height : 0;
            BroadcastPresence(); break;
        }
        case ControlType::RequestKeyframe: {
            std::uint32_t targetId{};
            if (!reader.U32(targetId) || reader.Remaining() != 0) { SendError(session, message.requestId, "Invalid keyframe request"); return; }
            const auto target = std::find_if(sessions.begin(), sessions.end(), [&](const auto& pair) {
                return pair.second.authenticated && pair.second.id == targetId && pair.second.channel == session.channel;
            });
            if (target == sessions.end() || !target->second.sharing) { SendError(session, message.requestId, "Sharing session not found"); return; }
            protocol::Writer payload; payload.U32(session.id);
            Send(target->second, ControlType::RequestKeyframe, message.requestId, payload.Data());
            break;
        }
        case ControlType::ChatMessage: {
            std::string text;
            if (!reader.String(text, 1000) || text.empty() || reader.Remaining() != 0) {
                SendError(session, message.requestId, "Invalid chat message");
                return;
            }
            protocol::Writer payload;
            payload.U32(session.id);
            payload.String(session.name, 32);
            payload.String(text, 1000);
            BroadcastChannel(session.channel, ControlType::ChatMessage, payload.Data());
            break;
        }
        default: SendError(session, message.requestId, "Message not permitted"); break;
        }
    }

    void HandleMessage(Session& session, const ControlMessage& message) {
        if (!session.authenticated) {
            if (message.type == ControlType::Hello) HandleHello(session, message);
            else SendError(session, message.requestId, "Send Hello first");
            return;
        }
        HandleAuthenticated(session, message);
    }

    void RelayMedia() {
        std::array<std::uint8_t, protocol::kMaxMediaPayload + 32> buffer{}; sockaddr_in sender{}; int senderSize = sizeof(sender);
        const int received = recvfrom(udpSocket, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0, reinterpret_cast<sockaddr*>(&sender), &senderSize);
        if (received <= 0) return;
        const auto packet = std::span<const std::uint8_t>(buffer.data(), static_cast<std::size_t>(received));
        const auto header = protocol::DecodeMediaHeader(packet);
        if (!header) return;
        const auto source = std::find_if(sessions.begin(), sessions.end(), [&](const auto& pair) {
            return pair.second.authenticated && pair.second.id == header->sessionId && pair.second.tcpAddress.sin_addr.s_addr == sender.sin_addr.s_addr;
        });
        if (source == sessions.end()) return;
        if (header->type == protocol::MediaType::Audio && source->second.muted) return;
        if (header->type == protocol::MediaType::Video && !source->second.sharing) return;
        for (const auto& [socket, recipient] : sessions) {
            if (!recipient.authenticated || recipient.id == source->second.id || recipient.channel != source->second.channel) continue;
            sendto(udpSocket, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0, reinterpret_cast<const sockaddr*>(&recipient.udpAddress), sizeof(recipient.udpAddress));
        }
    }
};

Server::Server(const std::uint16_t port) : port_(port), impl_(std::make_unique<Impl>()) {}
Server::~Server() { RequestStop(); }
void Server::RequestStop() noexcept { stop_.store(true); }

bool Server::Run() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) { std::cerr << "WSAStartup failed\n"; return false; }
    const auto cleanup = [this] { for (const auto& [socket, session] : impl_->sessions) closesocket(socket); impl_->sessions.clear(); if (impl_->tcpListener != INVALID_SOCKET) closesocket(impl_->tcpListener); if (impl_->udpSocket != INVALID_SOCKET) closesocket(impl_->udpSocket); WSACleanup(); };
    impl_->tcpListener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); impl_->udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (impl_->tcpListener == INVALID_SOCKET || impl_->udpSocket == INVALID_SOCKET) { std::cerr << "Socket creation failed\n"; cleanup(); return false; }
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_ANY); address.sin_port = htons(port_);
    if (bind(impl_->tcpListener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR || listen(impl_->tcpListener, SOMAXCONN) == SOCKET_ERROR || bind(impl_->udpSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) { std::cerr << "Cannot bind TCP/UDP port " << port_ << " (error " << WSAGetLastError() << ")\n"; cleanup(); return false; }
    std::cout << "VibeZ server listening on TCP/UDP " << port_ << "\n";
    const auto addresses = LocalIPv4Addresses();
    if (addresses.empty()) {
        std::cout << "No LAN IPv4 address found. Local-only address: 127.0.0.1:" << port_ << "\n";
    } else {
        std::cout << "Friends on the same network should connect to:\n";
        for (const auto& ip : addresses) std::cout << "  " << ip << ':' << port_ << "\n";
        std::cout << "For friends outside your network, use your public IP and forward TCP/UDP port " << port_ << ".\n";
    }
    std::cout << std::flush;
    while (!stop_.load()) {
        fd_set readable; FD_ZERO(&readable); FD_SET(impl_->tcpListener, &readable); FD_SET(impl_->udpSocket, &readable);
        for (const auto& [socket, session] : impl_->sessions) FD_SET(socket, &readable);
        timeval timeout{0, 200000};
        if (select(0, &readable, nullptr, nullptr, &timeout) == SOCKET_ERROR) break;
        if (FD_ISSET(impl_->tcpListener, &readable)) {
            sockaddr_in remote{}; int size = sizeof(remote); const SOCKET socket = accept(impl_->tcpListener, reinterpret_cast<sockaddr*>(&remote), &size);
            if (socket != INVALID_SOCKET) impl_->sessions.emplace(socket, Session{socket, remote});
        }
        if (FD_ISSET(impl_->udpSocket, &readable)) impl_->RelayMedia();
        std::vector<SOCKET> disconnect;
        for (auto& [socket, session] : impl_->sessions) {
            if (!FD_ISSET(socket, &readable)) continue;
            std::array<std::uint8_t, 4096> input{}; const int received = recv(socket, reinterpret_cast<char*>(input.data()), static_cast<int>(input.size()), 0);
            if (received <= 0) { disconnect.push_back(socket); continue; }
            session.received.insert(session.received.end(), input.begin(), input.begin() + received);
            while (true) { const auto message = protocol::TryReadControl(session.received); if (!message) break; impl_->HandleMessage(session, *message); }
        }
        const bool hadDisconnect = !disconnect.empty();
        for (const auto socket : disconnect) impl_->CloseSession(socket);
        if (hadDisconnect) impl_->BroadcastPresence();
    }
    cleanup();
    return true;
}
} // namespace vibez::server
