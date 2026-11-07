#include "server/include/vibez/server/Server.hpp"

#include <charconv>
#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    std::uint16_t port = 28765;
    if (argc == 2) {
        unsigned value{}; const std::string_view input(argv[1]);
        const auto [pointer, error] = std::from_chars(input.data(), input.data() + input.size(), value);
        if (error != std::errc{} || pointer != input.data() + input.size() || value == 0 || value > UINT16_MAX) { std::cerr << "Usage: VibeZServer [port]\n"; return 2; }
        port = static_cast<std::uint16_t>(value);
    } else if (argc > 2) { std::cerr << "Usage: VibeZServer [port]\n"; return 2; }
    vibez::server::Server server(port);
    return server.Run() ? 0 : 1;
}
