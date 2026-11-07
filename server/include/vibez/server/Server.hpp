#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

namespace vibez::server {

class Server {
public:
    explicit Server(std::uint16_t port);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Blocks on the control/media event loop until RequestStop is called or startup fails.
    [[nodiscard]] bool Run();
    void RequestStop() noexcept;

private:
    struct Impl;
    std::uint16_t port_;
    std::atomic_bool stop_{};
    std::unique_ptr<Impl> impl_;
};

} // namespace vibez::server
