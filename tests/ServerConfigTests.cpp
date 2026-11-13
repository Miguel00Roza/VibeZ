#include "server/src/ServerConfig.hpp"
#include "tests/TestSupport.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

using vibez::server::ServerConfig;

std::filesystem::path WriteTempFile(const std::string_view name, const std::string& contents) {
    const auto path = std::filesystem::temp_directory_path() / std::filesystem::path(std::string("vibez-config-") + std::string(name));
    std::ofstream output(path, std::ios::trunc);
    output << contents;
    output.close();
    return path;
}

void TestMissingFileKeepsDefaults() {
    ServerConfig config;
    std::string warning;
    const auto path = std::filesystem::temp_directory_path() / "vibez-config-does-not-exist.ini";
    std::filesystem::remove(path);
    CHECK(vibez::server::LoadServerConfig(path, config, warning));
    CHECK(warning.empty());
    CHECK(config.voicePort == 28765);
    CHECK(config.webPort == 8080);
    CHECK(config.webBindAddress == "0.0.0.0");
    CHECK(config.webEnabled);
}

void TestReadsEverything() {
    const auto path = WriteTempFile("full.ini",
                                    "; comentario\n"
                                    "# outro comentario\n"
                                    "\n"
                                    "[server]\r\n"
                                    "  voice_port = 29000  \r\n"
                                    "web_port=29001\r\n"
                                    "web_bind=127.0.0.1\r\n"
                                    "web_enabled=false\r\n");

    ServerConfig config;
    std::string warning;
    CHECK_MSG(vibez::server::LoadServerConfig(path, config, warning), warning);
    CHECK(warning.empty());
    CHECK(config.voicePort == 29000);
    CHECK(config.webPort == 29001);
    CHECK(config.webBindAddress == "127.0.0.1");
    CHECK(!config.webEnabled);
    std::filesystem::remove(path);
}

void TestGlobalKeysAndUnknownKeys() {
    // Keys before any section header belong to [server]; unknown keys are
    // ignored so a newer file still works with an older server.
    const auto path = WriteTempFile("global.ini",
                                    "voice_port=29010\n"
                                    "web_port=29011\n"
                                    "future_option=42\n"
                                    "\n"
                                    "[outra]\n"
                                    "voice_port=1\n");
    ServerConfig config;
    std::string warning;
    CHECK_MSG(vibez::server::LoadServerConfig(path, config, warning), warning);
    CHECK(warning.empty());
    CHECK(config.voicePort == 29010);
    CHECK(config.webPort == 29011);
    std::filesystem::remove(path);
}

void TestInvalidValuesKeepDefaults() {
    const auto path = WriteTempFile("invalid.ini",
                                    "[server]\n"
                                    "voice_port=0\n"
                                    "web_port=70000\n"
                                    "web_bind=\n"
                                    "web_enabled=maybe\n"
                                    "linha_sem_igual\n");
    ServerConfig config;
    std::string warning;
    CHECK(!vibez::server::LoadServerConfig(path, config, warning));
    CHECK(!warning.empty());
    CHECK(config.voicePort == 28765);
    CHECK(config.webPort == 8080);
    CHECK(config.webBindAddress == "0.0.0.0");
    CHECK(config.webEnabled);
    // The valid keys in the same file still apply.
    CHECK(warning.find("voice_port") != std::string::npos);
    CHECK(warning.find("web_port") != std::string::npos);
    std::filesystem::remove(path);
}

void TestPartialFileAppliesValidKeys() {
    const auto path = WriteTempFile("partial.ini",
                                    "[server]\n"
                                    "voice_port=29020\n"
                                    "web_port=abc\n");
    ServerConfig config;
    std::string warning;
    CHECK(!vibez::server::LoadServerConfig(path, config, warning));
    CHECK(config.voicePort == 29020);
    CHECK(config.webPort == 8080);
    std::filesystem::remove(path);
}

void TestWebPortZeroDisablesViewer() {
    const auto path = WriteTempFile("noweb.ini", "[server]\nweb_port=0\n");
    ServerConfig config;
    std::string warning;
    CHECK_MSG(vibez::server::LoadServerConfig(path, config, warning), warning);
    CHECK(config.webPort == 0);
    CHECK(!config.webEnabled);
    std::filesystem::remove(path);
}

void TestSaveRoundTrip() {
    const auto path = std::filesystem::temp_directory_path() / "vibez-config-round-trip" / "vibez-server.ini";
    std::filesystem::remove_all(path.parent_path());

    ServerConfig written;
    written.voicePort = 29030;
    written.webPort = 29031;
    written.webBindAddress = "192.168.0.10";
    written.webEnabled = false;

    std::string error;
    CHECK_MSG(vibez::server::SaveServerConfig(path, written, error), error);
    CHECK(std::filesystem::exists(path));

    ServerConfig loaded;
    std::string warning;
    CHECK_MSG(vibez::server::LoadServerConfig(path, loaded, warning), warning);
    CHECK(loaded.voicePort == written.voicePort);
    CHECK(loaded.webPort == written.webPort);
    CHECK(loaded.webBindAddress == written.webBindAddress);
    CHECK(!loaded.webEnabled);

    const auto description = vibez::server::DescribeServerConfig(loaded, path);
    CHECK(description.find("voice_port=29030") != std::string::npos);
    CHECK(description.find("web_port=29031") != std::string::npos);
    CHECK(description.find("web_enabled=0") != std::string::npos);
    CHECK(description.find(path.string()) != std::string::npos);

    std::filesystem::remove_all(path.parent_path());
}

} // namespace

int main() {
    TestMissingFileKeepsDefaults();
    TestReadsEverything();
    TestGlobalKeysAndUnknownKeys();
    TestInvalidValuesKeepDefaults();
    TestPartialFileAppliesValidKeys();
    TestWebPortZeroDisablesViewer();
    TestSaveRoundTrip();
    return vibez::test::Finish("vibez_server_config_tests");
}
