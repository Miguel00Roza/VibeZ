#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace vibez::server {

// Browser viewer configuration. The web server is disabled unless the caller
// explicitly enables it before Run().
struct WebConfig {
    std::uint16_t port{8080};
    std::string bindAddress{"0.0.0.0"};
    std::size_t maxViewers{16};
};

class Server {
public:
    explicit Server(std::uint16_t port);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Blocks on the control/media event loop until RequestStop is called or startup fails.
    [[nodiscard]] bool Run();
    void RequestStop() noexcept;

    // Starts serving the shared screen to browsers on the given port.
    void SetWebConfig(const WebConfig& config);

    // Actual port bound by the web viewer; 0 while it is disabled or stopped.
    [[nodiscard]] std::uint16_t WebPort() const noexcept;

private:
    struct Impl;
    std::uint16_t port_;
    std::atomic_bool stop_{};
    WebConfig webConfig_{};
    bool webEnabled_{};
    std::atomic<std::uint16_t> webPort_{0};
    std::unique_ptr<Impl> impl_;
};

} // namespace vibez::server
