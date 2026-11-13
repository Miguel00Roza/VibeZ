#include "server/src/ServerConfig.hpp"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <system_error>
#include <vector>

namespace vibez::server {
namespace {

std::string Trim(const std::string_view text) {
    const auto isSpace = [](const unsigned char ch) { return std::isspace(ch) != 0; };
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && isSpace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && isSpace(static_cast<unsigned char>(text[end - 1]))) --end;
    return std::string(text.substr(begin, end - begin));
}

std::string ToLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text;
}

bool ParseUnsigned(const std::string& text, unsigned& value) {
    if (text.empty()) return false;
    unsigned parsed{};
    const auto [pointer, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error != std::errc{} || pointer != text.data() + text.size()) return false;
    value = parsed;
    return true;
}

bool ParseBool(const std::string& text, bool& value) {
    const auto lowered = ToLower(text);
    if (lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on") {
        value = true;
        return true;
    }
    if (lowered == "0" || lowered == "false" || lowered == "no" || lowered == "off") {
        value = false;
        return true;
    }
    return false;
}

void AppendWarning(std::string& warning, const std::string& text) {
    if (!warning.empty()) warning += "; ";
    warning += text;
}

} // namespace

std::filesystem::path DefaultConfigPath() {
    std::vector<wchar_t> buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size()) {
            buffer.resize(length);
            break;
        }
        // Truncated: grow and retry, as documented for GetModuleFileNameW.
        buffer.resize(buffer.size() * 2);
    }
    const std::filesystem::path executable(buffer.begin(), buffer.end());
    return executable.parent_path() / L"vibez-server.ini";
}

bool LoadServerConfig(const std::filesystem::path& path, ServerConfig& config, std::string& warning) {
    warning.clear();
    if (path.empty()) return true;

    std::error_code statusError;
    if (!std::filesystem::exists(path, statusError)) return true;

    std::ifstream input(path);
    if (!input) {
        AppendWarning(warning, "nao foi possivel ler " + path.string());
        return false;
    }

    // Keys written before the first section header count as [server] keys, which
    // keeps hand-written files forgiving. Keys inside other sections are ignored.
    bool inServerSection = true;
    bool ok = true;
    std::string line;
    int lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed.front() == ';' || trimmed.front() == '#') continue;

        if (trimmed.front() == '[') {
            const auto close = trimmed.find(']');
            const std::string name = close == std::string::npos
                                         ? trimmed.substr(1)
                                         : trimmed.substr(1, close - 1);
            inServerSection = ToLower(Trim(name)) == "server";
            continue;
        }
        if (!inServerSection) continue;

        const auto separator = trimmed.find('=');
        if (separator == std::string::npos) {
            AppendWarning(warning, "linha " + std::to_string(lineNumber) + " sem '=' ignorada");
            ok = false;
            continue;
        }

        const std::string key = ToLower(Trim(trimmed.substr(0, separator)));
        const std::string value = Trim(trimmed.substr(separator + 1));
        if (key == "voice_port") {
            unsigned port{};
            if (!ParseUnsigned(value, port) || port == 0 || port > 65535) {
                AppendWarning(warning, "voice_port invalida (" + value + "), mantendo " +
                                           std::to_string(config.voicePort));
                ok = false;
                continue;
            }
            config.voicePort = static_cast<std::uint16_t>(port);
        } else if (key == "web_port") {
            unsigned port{};
            if (!ParseUnsigned(value, port) || port > 65535) {
                AppendWarning(warning, "web_port invalida (" + value + "), mantendo " +
                                           std::to_string(config.webPort));
                ok = false;
                continue;
            }
            config.webPort = static_cast<std::uint16_t>(port);
            // Port 0 has always meant "no browser viewer".
            config.webEnabled = port != 0;
        } else if (key == "web_bind") {
            if (value.empty()) {
                AppendWarning(warning, "web_bind vazio ignorado");
                ok = false;
                continue;
            }
            config.webBindAddress = value;
        } else if (key == "web_enabled") {
            bool enabled{};
            if (!ParseBool(value, enabled)) {
                AppendWarning(warning, "web_enabled invalido (" + value + "), mantendo o atual");
                ok = false;
                continue;
            }
            config.webEnabled = enabled;
        }
        // Unknown keys are ignored on purpose: a newer server may know them, and
        // an older one must still start.
    }
    return ok;
}

bool SaveServerConfig(const std::filesystem::path& path, const ServerConfig& config, std::string& error) {
    error.clear();
    if (path.empty()) {
        error = "caminho de configuracao vazio";
        return false;
    }
    std::error_code directoryError;
    const auto parent = path.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, directoryError);

    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        error = "nao foi possivel gravar " + path.string();
        return false;
    }
    output << "; VibeZ Server - gerado pelo instalador. Edite e reinicie o servidor.\n"
           << "[server]\n"
           << "voice_port=" << config.voicePort << '\n'
           << "web_port=" << config.webPort << '\n'
           << "web_bind=" << config.webBindAddress << '\n'
           << "web_enabled=" << (config.webEnabled ? 1 : 0) << '\n';
    if (!output.flush()) {
        error = "falha ao gravar " + path.string();
        return false;
    }
    return true;
}

std::string DescribeServerConfig(const ServerConfig& config, const std::filesystem::path& path) {
    std::string text;
    text += "config=" + path.string() + "\n";
    text += "voice_port=" + std::to_string(config.voicePort) + "\n";
    text += "web_port=" + std::to_string(config.webPort) + "\n";
    text += "web_bind=" + config.webBindAddress + "\n";
    text += std::string("web_enabled=") + (config.webEnabled ? "1" : "0") + "\n";
    return text;
}

} // namespace vibez::server
