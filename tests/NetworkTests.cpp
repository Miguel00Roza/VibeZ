#include "client/net/NetworkClient.hpp"
#include "common/protocol/Protocol.hpp"
#include "server/include/vibez/server/Server.hpp"
#include "tests/TestSupport.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {
using vibez::client::ConnectionState;
using vibez::client::MediaPacket;
using vibez::client::NetworkClient;
using vibez::client::NetworkConfig;
using vibez::client::NetworkEvent;
using vibez::protocol::ControlType;

std::uint16_t BoundPort(const SOCKET socket) {
    sockaddr_in address{};
    int size = sizeof(address);
    return getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size) == 0 ? ntohs(address.sin_port) : 0;
}

std::uint16_t FindFreePort() {
    const SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) return 0;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        closesocket(socket);
        return 0;
    }
    const auto port = BoundPort(socket);
    closesocket(socket);
    return port;
}

NetworkConfig ConfigFor(const std::uint16_t port, const std::string& nickname) {
    NetworkConfig config;
    config.host = "127.0.0.1";
    config.port = port;
    config.nickname = nickname;
    config.autoReconnect = true;
    return config;
}

std::optional<NetworkEvent> WaitFor(NetworkClient& client, const std::function<bool(const NetworkEvent&)>& predicate,
                                    const int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        while (const auto event = client.TryPopEvent()) {
            if (predicate(*event)) return event;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return std::nullopt;
}

bool WaitForState(NetworkClient& client, const ConnectionState state, const int timeoutMs) {
    return WaitFor(client, [state](const NetworkEvent& event) {
        return event.type == NetworkEvent::Type::StateChanged && event.state == state;
    }, timeoutMs).has_value();
}

bool PresenceHasUser(const NetworkEvent& event, const std::uint32_t userId, const std::uint32_t channelId) {
    if (event.type != NetworkEvent::Type::PresenceUpdated) return false;
    for (const auto& user : event.users) {
        if (user.id == userId && user.channel == channelId) return true;
    }
    return false;
}

bool PresenceHasSharing(const NetworkEvent& event, const std::uint32_t userId) {
    if (event.type != NetworkEvent::Type::PresenceUpdated) return false;
    for (const auto& user : event.users) {
        if (user.id == userId && user.sharing && user.shareWidth == 640 && user.shareHeight == 480) return true;
    }
    return false;
}
} // namespace

int main() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;
    const auto port = FindFreePort();
    CHECK(port != 0);
    if (port == 0) {
        WSACleanup();
        return vibez::test::Finish("Network tests");
    }

    // Phase 1: connect attempts fail while the server is down; the client must
    // retry and then complete the Hello handshake once the server appears.
    NetworkClient alice;
    std::string error;
    CHECK(alice.Start(ConfigFor(port, "Alice"), error));
    CHECK(error.empty());
    CHECK(WaitForState(alice, ConnectionState::Reconnecting, 8000));

    vibez::server::Server server(port);
    std::thread serverThread([&] { (void)server.Run(); });
    const auto aliceHello = WaitFor(alice, [](const NetworkEvent& event) {
        return event.type == NetworkEvent::Type::HelloAccepted && event.sessionId != 0;
    }, 10000);
    CHECK_MSG(aliceHello.has_value(), "Alice did not reconnect after the server started");
    const std::uint32_t aliceId = aliceHello ? aliceHello->sessionId : 0;
    CHECK(aliceId != 0);

    // Phase 2: a second client connects and both receive the presence list.
    NetworkClient bob;
    std::mutex packetMutex;
    std::optional<MediaPacket> receivedAudio;
    bob.SetAudioSink([&](const MediaPacket& packet) {
        std::lock_guard lock(packetMutex);
        receivedAudio = packet;
    });
    CHECK(bob.Start(ConfigFor(port, "Bob"), error));
    const auto bobHello = WaitFor(bob, [](const NetworkEvent& event) {
        return event.type == NetworkEvent::Type::HelloAccepted && event.sessionId != 0;
    }, 5000);
    CHECK_MSG(bobHello.has_value(), "Bob did not receive HelloAccepted");
    const std::uint32_t bobId = bobHello ? bobHello->sessionId : 0;
    CHECK(bobId != 0 && bobId != aliceId);

    if (aliceId != 0 && bobId != 0) {
        // Phase 3: channel creation and channel presence.
        std::uint32_t channelId = 0;
        vibez::protocol::Writer createPayload;
        createPayload.String("Friends", 64);
        CHECK(alice.SendControl(ControlType::CreateChannel, createPayload.Data()));
        const auto channels = WaitFor(alice, [&](const NetworkEvent& event) {
            if (event.type != NetworkEvent::Type::ChannelsUpdated) return false;
            for (const auto& channel : event.channels) {
                if (channel.name == "Friends") {
                    channelId = channel.id;
                    return true;
                }
            }
            return false;
        }, 3000);
        CHECK_MSG(channels.has_value(), "Friends channel was not announced");
        CHECK(channelId != 0);

        if (channelId != 0) {
            vibez::protocol::Writer joinPayload;
            joinPayload.U32(channelId);
            CHECK(alice.SendControl(ControlType::JoinChannel, joinPayload.Data()));
            CHECK(bob.SendControl(ControlType::JoinChannel, joinPayload.Data()));
            CHECK(WaitFor(bob, [&](const NetworkEvent& event) {
                return PresenceHasUser(event, aliceId, channelId) && PresenceHasUser(event, bobId, channelId);
            }, 3000).has_value());

            // Phase 4: UDP audio relay from Alice to Bob.
            const std::vector<std::uint8_t> audioPayload{1, 2, 3, 4, 5};
            CHECK(alice.SendAudio(7, 1234, audioPayload));
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
            bool arrived = false;
            while (std::chrono::steady_clock::now() < deadline) {
                {
                    std::lock_guard lock(packetMutex);
                    arrived = receivedAudio.has_value();
                }
                if (arrived) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            CHECK_MSG(arrived, "Relayed audio packet did not arrive at Bob");
            {
                std::lock_guard lock(packetMutex);
                if (receivedAudio) {
                    CHECK(receivedAudio->type == vibez::protocol::MediaType::Audio);
                    CHECK(receivedAudio->sessionId == aliceId);
                    CHECK(receivedAudio->sequence == 7);
                    CHECK(receivedAudio->payload == audioPayload);
                }
            }

            // Phase 5: invalid control payloads produce Error events.
            CHECK(bob.SendControl(ControlType::SetSharing, {}));
            const auto invalidSharing = WaitFor(bob, [](const NetworkEvent& event) {
                return event.type == NetworkEvent::Type::Error && event.text.find("sharing") != std::string::npos;
            }, 3000);
            CHECK_MSG(invalidSharing.has_value(), "Invalid sharing payload did not produce an Error event");

            // Phase 6: keyframe requests require the target to be sharing.
            CHECK(bob.RequestKeyframe(aliceId));
            const auto notSharing = WaitFor(bob, [](const NetworkEvent& event) {
                return event.type == NetworkEvent::Type::Error && event.text.find("Sharing") != std::string::npos;
            }, 3000);
            CHECK_MSG(notSharing.has_value(), "Keyframe request for a non-sharing target was not rejected");

            vibez::protocol::Writer sharingPayload;
            sharingPayload.U8(1);
            sharingPayload.U16(640);
            sharingPayload.U16(480);
            CHECK(alice.SendControl(ControlType::SetSharing, sharingPayload.Data()));
            CHECK(WaitFor(bob, [&](const NetworkEvent& event) { return PresenceHasSharing(event, aliceId); }, 3000).has_value());
            CHECK(bob.RequestKeyframe(aliceId));
            const auto keyframe = WaitFor(alice, [&](const NetworkEvent& event) {
                return event.type == NetworkEvent::Type::KeyframeRequest && event.sessionId == bobId;
            }, 3000);
            CHECK_MSG(keyframe.has_value(), "Keyframe request was not forwarded to the sharing client");

            // Phase 7: chat is delivered only through the server to the current channel.
            CHECK(alice.SendChat("Hello from Alice"));
            const auto chat = WaitFor(bob, [&](const NetworkEvent& event) {
                return event.type == NetworkEvent::Type::ChatMessage && event.sessionId == aliceId &&
                       event.senderName == "Alice" && event.text == "Hello from Alice";
            }, 3000);
            CHECK_MSG(chat.has_value(), "Channel chat message did not arrive at Bob");
        }
    }

    // Phase 8: server shutdown must put the client back into reconnecting.
    server.RequestStop();
    serverThread.join();
    CHECK(WaitForState(alice, ConnectionState::Reconnecting, 8000));

    alice.Stop();
    bob.Stop();
    CHECK(!alice.IsRunning());
    CHECK(!bob.IsRunning());
    WSACleanup();
    return vibez::test::Finish("Network tests");
}
