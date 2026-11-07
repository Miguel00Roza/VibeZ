#include "client/core/ClientSession.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
const char* StateText(const vibez::client::ConnectionState state) {
    switch (state) {
    case vibez::client::ConnectionState::Disconnected: return "disconnected";
    case vibez::client::ConnectionState::Connecting: return "connecting";
    case vibez::client::ConnectionState::Connected: return "connected";
    case vibez::client::ConnectionState::Reconnecting: return "reconnecting";
    }
    return "unknown";
}

bool ParseU32(const std::string_view text, std::uint32_t& value) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size();
}

void PrintEvent(const vibez::client::NetworkEvent& event, std::vector<vibez::client::UserInfo>& users) {
    using vibez::client::NetworkEvent;
    switch (event.type) {
    case NetworkEvent::Type::StateChanged:
        std::cout << "Network: " << StateText(event.state) << "\n";
        break;
    case NetworkEvent::Type::HelloAccepted:
        std::cout << "Connected as #" << event.sessionId << " in channel " << event.channelId << "\n";
        break;
    case NetworkEvent::Type::ChannelsUpdated:
        std::cout << "Channels:";
        for (const auto& channel : event.channels) std::cout << " [" << channel.id << ": " << channel.name << ']';
        std::cout << "\n";
        break;
    case NetworkEvent::Type::PresenceUpdated:
        users = event.users;
        std::cout << "Users:";
        for (const auto& user : event.users) {
            std::cout << " [" << user.name << " #" << user.id << " c" << user.channel;
            if (user.muted) std::cout << " muted";
            if (user.sharing) std::cout << " sharing " << user.shareWidth << 'x' << user.shareHeight;
            std::cout << ']';
        }
        std::cout << "\n";
        break;
    case NetworkEvent::Type::Error:
        std::cerr << "Server error: " << event.text << "\n";
        break;
    case NetworkEvent::Type::KeyframeRequest:
        std::cout << "Keyframe requested by #" << event.sessionId << "\n";
        break;
    }
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 3 || argc > 4) {
        std::cerr << "Usage: VibeZ <server-ipv4> <port> [nickname]\n";
        return 2;
    }
    std::uint32_t parsedPort{};
    if (!ParseU32(argv[2], parsedPort) || parsedPort == 0 || parsedPort > UINT16_MAX) {
        std::cerr << "Invalid port\n";
        return 2;
    }
    const std::string nickname = argc == 4 ? argv[3] : "Guest";
    if (nickname.empty() || nickname.size() > 32) {
        std::cerr << "Nickname must contain 1 to 32 bytes\n";
        return 2;
    }

    vibez::client::ClientSessionConfig config;
    config.network.host = argv[1];
    config.network.port = static_cast<std::uint16_t>(parsedPort);
    config.network.nickname = nickname;
    config.network.token = "";
    config.network.autoReconnect = true;
    config.enableAudio = true;

    vibez::client::ClientSession session;
    std::string error;
    if (!session.Start(config, error)) {
        std::cerr << "Could not start the client: " << error << "\n";
        return 1;
    }
    if (!session.AudioRunning()) {
        std::cerr << "Audio unavailable: " << session.AudioError() << "\n";
    }
    std::cout << "Commands: /create <name>, /join <id>, /leave, /mute <0|1>, /share <0|1> <width> <height>,\n"
                 "          /keyframe <session-id>, /level, /quit\n";

    std::mutex inputMutex;
    std::vector<std::string> inputLines;
    std::atomic_bool inputClosed{};
    std::thread reader([&] {
        std::string line;
        while (std::getline(std::cin, line)) {
            std::lock_guard lock(inputMutex);
            inputLines.push_back(std::move(line));
        }
        inputClosed.store(true);
    });
    reader.detach();

    std::vector<vibez::client::UserInfo> users;
    bool quit = false;
    while (!quit) {
        std::vector<std::string> lines;
        {
            std::lock_guard lock(inputMutex);
            lines.swap(inputLines);
        }
        for (const auto& line : lines) {
            if (line == "/quit") { quit = true; break; }
            if (line == "/leave") {
                if (!session.LeaveChannel()) std::cerr << "Could not send /leave\n";
                continue;
            }
            if (line == "/level") {
                std::cout << "Level local=" << session.LocalLevel() << " rx=" << session.ReceivedPackets();
                for (const auto& user : users) std::cout << ' ' << user.name << '=' << session.UserLevel(user.id);
                std::cout << "\n";
                continue;
            }
            if (line.rfind("/create ", 0) == 0) {
                if (!session.CreateChannel(line.substr(8))) std::cerr << "Could not send /create\n";
                continue;
            }
            if (line.rfind("/join ", 0) == 0) {
                std::uint32_t id{};
                if (!ParseU32(std::string_view(line).substr(6), id) || id == 0) { std::cerr << "Invalid channel id\n"; continue; }
                if (!session.JoinChannel(id)) std::cerr << "Could not send /join\n";
                continue;
            }
            if (line.rfind("/mute ", 0) == 0) {
                const auto value = std::string_view(line).substr(6);
                if (value != "0" && value != "1") { std::cerr << "Use /mute 0 or /mute 1\n"; continue; }
                if (!session.SetMicrophoneMuted(value == "1")) std::cerr << "Could not send /mute\n";
                continue;
            }
            if (line.rfind("/share ", 0) == 0) {
                std::istringstream input(line.substr(7));
                int enabled{};
                int width{};
                int height{};
                if (!(input >> enabled >> width >> height) || (enabled != 0 && enabled != 1) ||
                    width < 0 || width > UINT16_MAX || height < 0 || height > UINT16_MAX) {
                    std::cerr << "Use /share <0|1> <width> <height>\n";
                    continue;
                }
                if (!session.SetSharing(enabled != 0, static_cast<std::uint16_t>(width), static_cast<std::uint16_t>(height))) {
                    std::cerr << "Could not send /share\n";
                }
                continue;
            }
            if (line.rfind("/keyframe ", 0) == 0) {
                std::uint32_t id{};
                if (!ParseU32(std::string_view(line).substr(10), id) || id == 0) { std::cerr << "Invalid session id\n"; continue; }
                if (!session.RequestKeyframe(id)) std::cerr << "Could not send /keyframe\n";
                continue;
            }
            std::cerr << "Unknown or invalid command\n";
        }

        while (const auto event = session.TryPopEvent()) PrintEvent(*event, users);
        if (!quit && inputClosed.load()) quit = true;
        if (!quit) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    session.Stop();
    std::cout.flush();
    std::cerr.flush();
    // The stdin reader is detached and may still be blocked in getline; skip
    // static destruction so process exit cannot race with it.
    std::_Exit(0);
}
