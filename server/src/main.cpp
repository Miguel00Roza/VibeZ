#include "server/include/vibez/server/Server.hpp"

#include <charconv>
#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

void PrintUsage() {
    std::cerr << "Usage: VibeZServer [port] [options]\n"
              << "  --web-port <port>   Browser viewer port (default 8080, 0 disables it)\n"
              << "  --web-bind <addr>   Address the viewer listens on (default 0.0.0.0)\n"
              << "  --no-web            Do not start the browser viewer\n"
              << "  --help              Show this help\n";
}

bool ParsePort(const std::string_view text, std::uint16_t& port) {
    unsigned value{};
    const auto [pointer, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || pointer != text.data() + text.size() || value == 0 || value > UINT16_MAX) return false;
    port = static_cast<std::uint16_t>(value);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::uint16_t port = 28765;
    vibez::server::WebConfig web;
    bool webEnabled = true;
    bool portSet = false;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help" || argument == "-h") {
            PrintUsage();
            return 0;
        }
        if (argument == "--no-web") {
            webEnabled = false;
            continue;
        }
        if (argument == "--web-port") {
            if (index + 1 >= argc) {
                PrintUsage();
                return 2;
            }
            const std::string_view value(argv[++index]);
            unsigned number{};
            const auto [pointer, error] = std::from_chars(value.data(), value.data() + value.size(), number);
            if (error != std::errc{} || pointer != value.data() + value.size() || number > UINT16_MAX) {
                PrintUsage();
                return 2;
            }
            web.port = static_cast<std::uint16_t>(number);
            webEnabled = web.port != 0;
            continue;
        }
        if (argument == "--web-bind") {
            if (index + 1 >= argc) {
                PrintUsage();
                return 2;
            }
            const std::string_view value(argv[++index]);
            if (value.empty()) {
                PrintUsage();
                return 2;
            }
            web.bindAddress.assign(value);
            continue;
        }
        if (!argument.empty() && argument.front() == '-') {
            PrintUsage();
            return 2;
        }
        if (portSet || !ParsePort(argument, port)) {
            PrintUsage();
            return 2;
        }
        portSet = true;
    }

    vibez::server::Server server(port);
    if (webEnabled) server.SetWebConfig(web);
    return server.Run() ? 0 : 1;
}
