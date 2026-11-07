#include "client/audio/AudioConstants.hpp"
#include "client/audio/AudioFormat.hpp"
#include "client/audio/AudioMixer.hpp"
#include "client/audio/OpusCodec.hpp"
#include "client/net/NetworkClient.hpp"
#include "common/protocol/Protocol.hpp"
#include "server/include/vibez/server/Server.hpp"
#include "tests/TestSupport.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {
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
    config.autoReconnect = false;
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
} // namespace

int main() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;
    const auto port = FindFreePort();
    CHECK(port != 0);
    if (port == 0) {
        WSACleanup();
        return vibez::test::Finish("Voice-over-network tests");
    }

    vibez::server::Server server(port);
    std::thread serverThread([&] { (void)server.Run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    NetworkClient alice;
    NetworkClient bob;
    std::string error;
    CHECK(alice.Start(ConfigFor(port, "Alice"), error));
    CHECK(bob.Start(ConfigFor(port, "Bob"), error));
    const auto aliceHello = WaitFor(alice, [](const NetworkEvent& event) {
        return event.type == NetworkEvent::Type::HelloAccepted && event.sessionId != 0;
    }, 5000);
    const auto bobHello = WaitFor(bob, [](const NetworkEvent& event) {
        return event.type == NetworkEvent::Type::HelloAccepted && event.sessionId != 0;
    }, 5000);
    CHECK(aliceHello.has_value());
    CHECK(bobHello.has_value());
    const std::uint32_t aliceId = aliceHello ? aliceHello->sessionId : 0;
    const std::uint32_t bobId = bobHello ? bobHello->sessionId : 0;

    // The receiving side runs the real Opus decoder and mixer; the network
    // worker thread feeds it exactly like ClientSession does.
    vibez::audio::VoiceDecoder decoder;
    vibez::audio::AudioMixer mixer;
    std::atomic<int> received{};
    std::mutex decodeMutex;
    bob.SetAudioSink([&](const MediaPacket& packet) {
        if (packet.type != vibez::protocol::MediaType::Audio) return;
        std::lock_guard lock(decodeMutex);
        const auto pcm = decoder.Decode(packet.payload);
        mixer.PushSamples(packet.sessionId, pcm);
        received.fetch_add(1);
    });

    if (aliceId != 0 && bobId != 0) {
        std::vector<std::int16_t> tone(vibez::audio::kFrameSamples);
        for (std::size_t i = 0; i < tone.size(); ++i) {
            tone[i] = static_cast<std::int16_t>(12000.0 * std::sin(static_cast<double>(i) * 0.11));
        }
        vibez::audio::VoiceEncoder encoder;
        const auto encoded = encoder.Encode(tone);
        CHECK(!encoded.empty());
        CHECK(alice.SendAudio(1, 1000, encoded));

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline && received.load() == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK_MSG(received.load() > 0, "Encoded voice frame did not reach Bob");

        std::vector<std::int16_t> mixed(static_cast<std::size_t>(vibez::audio::kFrameSamples));
        {
            std::lock_guard lock(decodeMutex);
            mixer.Mix(mixed);
        }
        const auto rms = vibez::audio::RmsLevel(mixed);
        CHECK_MSG(rms > 0.02F, "Decoded voice frame is silent");
        CHECK(mixer.PeakLevel(aliceId) > 0.02F);

        // Channel isolation: move Bob elsewhere and verify audio stops.
        vibez::protocol::Writer createPayload;
        createPayload.String("Elsewhere", 64);
        CHECK(alice.SendControl(ControlType::CreateChannel, createPayload.Data()));
        std::uint32_t otherChannel = 0;
        CHECK(WaitFor(alice, [&](const NetworkEvent& event) {
            if (event.type != NetworkEvent::Type::ChannelsUpdated) return false;
            for (const auto& channel : event.channels) {
                if (channel.name == "Elsewhere") {
                    otherChannel = channel.id;
                    return true;
                }
            }
            return false;
        }, 3000).has_value());

        if (otherChannel != 0) {
            vibez::protocol::Writer joinPayload;
            joinPayload.U32(otherChannel);
            CHECK(bob.SendControl(ControlType::JoinChannel, joinPayload.Data()));
            CHECK(WaitFor(bob, [&](const NetworkEvent& event) {
                if (event.type != NetworkEvent::Type::PresenceUpdated) return false;
                for (const auto& user : event.users) {
                    if (user.id == bobId && user.channel == otherChannel) return true;
                }
                return false;
            }, 3000).has_value());

            received.store(0);
            CHECK(alice.SendAudio(2, 1020, encoded));
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
            CHECK_MSG(received.load() == 0, "Audio crossed channel boundaries");
        }
    }

    alice.Stop();
    bob.Stop();
    server.RequestStop();
    serverThread.join();
    WSACleanup();
    return vibez::test::Finish("Voice-over-network tests");
}
