#include "common/protocol/Protocol.hpp"
#include "server/include/vibez/server/Server.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <array>
#include <chrono>
#include <functional>
#include <iostream>
#include <thread>
#include <vector>

namespace {
using namespace vibez::protocol;

struct Client { SOCKET tcp{INVALID_SOCKET}; SOCKET udp{INVALID_SOCKET}; std::vector<std::uint8_t> stream; std::uint32_t id{}; };

void Close(Client& client) { if (client.tcp != INVALID_SOCKET) closesocket(client.tcp); if (client.udp != INVALID_SOCKET) closesocket(client.udp); }
bool SendControl(const SOCKET socket, const ControlMessage& message) { const auto bytes = EncodeControl(message); return !bytes.empty() && send(socket, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0) == static_cast<int>(bytes.size()); }
std::uint16_t BoundPort(const SOCKET socket) { sockaddr_in address{}; int size = sizeof(address); return getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size) == 0 ? ntohs(address.sin_port) : 0; }

std::uint16_t FindFreePort() {
    const SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); if (socket == INVALID_SOCKET) return 0;
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) { closesocket(socket); return 0; }
    const auto port = BoundPort(socket); closesocket(socket); return port;
}

bool Connect(Client& client, const std::uint16_t port, const std::string& name) {
    client.tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); client.udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (client.tcp == INVALID_SOCKET || client.udp == INVALID_SOCKET) return false;
    sockaddr_in local{}; local.sin_family = AF_INET; local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(client.udp, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) return false;
    sockaddr_in server{}; server.sin_family = AF_INET; server.sin_addr.s_addr = htonl(INADDR_LOOPBACK); server.sin_port = htons(port);
    if (connect(client.tcp, reinterpret_cast<sockaddr*>(&server), sizeof(server)) == SOCKET_ERROR) return false;
    Writer hello; if (!hello.String(name, 32)) return false; hello.U16(BoundPort(client.udp)); hello.String("", 128);
    return SendControl(client.tcp, {ControlType::Hello, 1, hello.Data()});
}

bool ReceiveUntil(Client& client, const std::function<bool(const ControlMessage&)>& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        while (const auto message = TryReadControl(client.stream)) if (predicate(*message)) return true;
        fd_set readable; FD_ZERO(&readable); FD_SET(client.tcp, &readable); timeval timeout{0, 100000};
        if (select(0, &readable, nullptr, nullptr, &timeout) <= 0) continue;
        std::array<std::uint8_t, 4096> bytes{}; const int count = recv(client.tcp, reinterpret_cast<char*>(bytes.data()), static_cast<int>(bytes.size()), 0);
        if (count <= 0) return false;
        client.stream.insert(client.stream.end(), bytes.begin(), bytes.begin() + count);
    }
    return false;
}

bool IsChannelSnapshotWith(const ControlMessage& message, const std::string& sought) {
    if (message.type != ControlType::ChannelSnapshot) return false;
    Reader reader(message.payload); std::uint32_t count{}; if (!reader.U32(count)) return false;
    for (std::uint32_t i = 0; i < count; ++i) { std::uint32_t id{}; std::string name; if (!reader.U32(id) || !reader.String(name, 64)) return false; if (name == sought) return true; }
    return false;
}

bool IsPresenceInChannel(const ControlMessage& message, const std::uint32_t userId, const std::uint32_t channelId) {
    if (message.type != ControlType::PresenceSnapshot) return false;
    Reader reader(message.payload); std::uint32_t count{}; if (!reader.U32(count)) return false;
    for (std::uint32_t i = 0; i < count; ++i) { std::uint32_t id{}, channel{}; std::uint8_t muted{}, sharing{}; std::uint16_t width{}, height{}; std::string name; if (!reader.U32(id) || !reader.U32(channel) || !reader.U8(muted) || !reader.U8(sharing) || !reader.U16(width) || !reader.U16(height) || !reader.String(name, 32)) return false; if (id == userId && channel == channelId) return true; }
    return false;
}
}

int main() {
    WSADATA data{}; if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;
    const auto port = FindFreePort(); if (port == 0) { WSACleanup(); return 1; }
    vibez::server::Server server(port); std::thread serverThread([&] { const bool completed = server.Run(); (void)completed; }); std::this_thread::sleep_for(std::chrono::milliseconds(150));
    Client alice, bob; bool passed = Connect(alice, port, "Alice") && Connect(bob, port, "Bob");
    if (passed) passed = ReceiveUntil(alice, [&](const ControlMessage& m) { if (m.type != ControlType::HelloAccepted) return false; Reader r(m.payload); std::uint32_t channel{}; return r.U32(alice.id) && r.U32(channel) && alice.id != 0; });
    if (passed) passed = ReceiveUntil(bob, [&](const ControlMessage& m) { if (m.type != ControlType::HelloAccepted) return false; Reader r(m.payload); std::uint32_t channel{}; return r.U32(bob.id) && r.U32(channel) && bob.id != 0; });
    if (passed) { Writer payload; passed = payload.String("Friends", 64) && SendControl(alice.tcp, {ControlType::CreateChannel, 2, payload.Data()}); }
    if (passed) passed = ReceiveUntil(alice, [](const ControlMessage& m) { return IsChannelSnapshotWith(m, "Friends"); });
    if (passed) { Writer payload; payload.U32(2); passed = SendControl(alice.tcp, {ControlType::JoinChannel, 3, payload.Data()}) && SendControl(bob.tcp, {ControlType::JoinChannel, 4, payload.Data()}); }
    if (passed) passed = ReceiveUntil(alice, [&](const ControlMessage& m) { return IsPresenceInChannel(m, bob.id, 2); });
    if (passed) {
        const auto header = EncodeMediaHeader({MediaType::Audio, alice.id, 1, 99, 3}); std::vector<std::uint8_t> media = header; media.insert(media.end(), {1, 2, 3});
        sockaddr_in serverAddress{}; serverAddress.sin_family = AF_INET; serverAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK); serverAddress.sin_port = htons(port);
        passed = sendto(alice.udp, reinterpret_cast<const char*>(media.data()), static_cast<int>(media.size()), 0, reinterpret_cast<sockaddr*>(&serverAddress), sizeof(serverAddress)) == static_cast<int>(media.size());
        fd_set readable; FD_ZERO(&readable); FD_SET(bob.udp, &readable); timeval timeout{3, 0};
        if (passed && select(0, &readable, nullptr, nullptr, &timeout) > 0) { std::array<std::uint8_t, 128> received{}; const int count = recv(bob.udp, reinterpret_cast<char*>(received.data()), static_cast<int>(received.size()), 0); const auto decoded = count > 0 ? DecodeMediaHeader(std::span<const std::uint8_t>(received.data(), static_cast<std::size_t>(count))) : std::nullopt; passed = decoded && decoded->sessionId == alice.id && decoded->payloadSize == 3; } else passed = false;
    }
    Close(alice); Close(bob); server.RequestStop(); serverThread.join(); WSACleanup();
    if (!passed) { std::cerr << "Server integration test failed\n"; return 1; }
    std::cout << "Server integration test passed\n";
}
