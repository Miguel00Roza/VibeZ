#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vibez::client {

enum class ConnectionState { Disconnected, Connecting, Connected, Reconnecting };

struct ChannelInfo {
    std::uint32_t id{};
    std::string name;
};

struct UserInfo {
    std::uint32_t id{};
    std::uint32_t channel{};
    bool muted{};
    bool sharing{};
    std::uint16_t shareWidth{};
    std::uint16_t shareHeight{};
    std::string name;
};

struct NetworkEvent {
    enum class Type { StateChanged, HelloAccepted, ChannelsUpdated, PresenceUpdated, Error, KeyframeRequest, ChatMessage };
    Type type{};
    ConnectionState state{ConnectionState::Disconnected};
    std::uint32_t sessionId{};
    std::uint32_t channelId{};
    std::uint32_t requestId{};
    std::string text;
    std::string senderName;
    std::vector<ChannelInfo> channels;
    std::vector<UserInfo> users;
};

} // namespace vibez::client
