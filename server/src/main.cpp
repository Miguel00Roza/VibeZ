#include "server/include/vibez/server/Server.hpp"
#include "server/src/ServerConfig.hpp"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

void PrintUsage() {
    std::cerr << "Usage: VibeZServer [port] [options]\n"
              << "  --voice-port <port>   Voice, control and video port (default 28765)\n"
              << "  --web-port <port>     Browser viewer port (default 8080, 0 disables it)\n"
              << "  --web-bind <addr>     Address the viewer listens on (default 0.0.0.0)\n"
              << "  --no-web              Do not start the browser viewer\n"
              << "  --config <file>       Settings file (default: vibez-server.ini beside the exe)\n"
              << "  --print-config        Print the effective settings and exit\n"
              << "  --help                Show this help\n"
              << "\nCommand line values override the settings file, which overrides the defaults.\n";
}

bool ParsePort(const std::string_view text, std::uint16_t& port, const bool allowZero) {
    unsigned value{};
    const auto [pointer, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || pointer != text.data() + text.size() || value > UINT16_MAX) return false;
    if (value == 0 && !allowZero) return false;
    port = static_cast<std::uint16_t>(value);
    return true;
}

std::optional<std::string_view> TakeValue(const int argc, char** argv, int& index) {
    if (index + 1 >= argc) return std::nullopt;
    return std::string_view(argv[++index]);
}

} // namespace

int main(int argc, char** argv) {
    const std::filesystem::path defaultConfigPath = vibez::server::DefaultConfigPath();

    // --config has to be resolved before the file is read, so it is located
    // first and then skipped by the main pass below.
    std::filesystem::path configPath = defaultConfigPath;
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) != "--config") continue;
        if (index + 1 >= argc) {
            PrintUsage();
            return 2;
        }
        configPath = argv[index + 1];
    }

    vibez::server::ServerConfig config;
    std::string warning;
    if (!vibez::server::LoadServerConfig(configPath, config, warning)) {
        std::cerr << "Settings: " << warning << "\n";
    }

    bool printConfig = false;
    bool voicePortSet = false;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help" || argument == "-h") {
            PrintUsage();
            return 0;
        }
        if (argument == "--config") {
            ++index; // Already consumed above.
            continue;
        }
        if (argument == "--print-config") {
            printConfig = true;
            continue;
        }
        if (argument == "--no-web") {
            config.webEnabled = false;
            continue;
        }
        if (argument == "--voice-port") {
            const auto value = TakeValue(argc, argv, index);
            if (!value || voicePortSet || !ParsePort(*value, config.voicePort, false)) {
                PrintUsage();
                return 2;
            }
            voicePortSet = true;
            continue;
        }
        if (argument == "--web-port") {
            const auto value = TakeValue(argc, argv, index);
            if (!value || !ParsePort(*value, config.webPort, true)) {
                PrintUsage();
                return 2;
            }
            // Port 0 disables the browser viewer, matching the historical flag.
            config.webEnabled = config.webPort != 0;
            continue;
        }
        if (argument == "--web-bind") {
            const auto value = TakeValue(argc, argv, index);
            if (!value || value->empty()) {
                PrintUsage();
                return 2;
            }
            config.webBindAddress.assign(*value);
            continue;
        }
        if (!argument.empty() && argument.front() == '-') {
            PrintUsage();
            return 2;
        }
        // The bare positional port is the voice port, kept for compatibility
        // with existing shortcuts and scripts.
        if (voicePortSet || !ParsePort(argument, config.voicePort, false)) {
            PrintUsage();
            return 2;
        }
        voicePortSet = true;
    }

    if (printConfig) {
        std::cout << vibez::server::DescribeServerConfig(config, configPath);
        return 0;
    }

    vibez::server::Server server(config.voicePort);
    if (config.webEnabled) {
        vibez::server::WebConfig web;
        web.port = config.webPort;
        web.bindAddress = config.webBindAddress;
        server.SetWebConfig(web);
    }
    return server.Run() ? 0 : 1;
}
