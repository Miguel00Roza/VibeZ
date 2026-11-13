#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace vibez::server {

// Effective server settings. Defaults match the historical hard-coded values so
// an existing deployment keeps working without any configuration file.
struct ServerConfig {
    std::uint16_t voicePort{28765};
    std::uint16_t webPort{8080};
    std::string webBindAddress{"0.0.0.0"};
    bool webEnabled{true};
};

// "vibez-server.ini" next to the running executable. Empty when the path cannot
// be resolved, in which case the caller simply keeps the defaults.
[[nodiscard]] std::filesystem::path DefaultConfigPath();

// Reads the INI file. A missing file is not an error: the defaults stay in
// place. Returns false when the file exists but contains something that had to
// be ignored, leaving `warning` with a human readable description. The config is
// always safe to use, even when this returns false.
bool LoadServerConfig(const std::filesystem::path& path, ServerConfig& config, std::string& warning);

// Writes the INI file, creating the parent directory when needed.
bool SaveServerConfig(const std::filesystem::path& path, const ServerConfig& config, std::string& error);

// Multi-line summary used by --print-config and by the installer test.
[[nodiscard]] std::string DescribeServerConfig(const ServerConfig& config, const std::filesystem::path& path);

} // namespace vibez::server
