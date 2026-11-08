#pragma once

#include "server/src/ScreenStream.hpp"
#include "server/src/SystemAudio.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace vibez::server {

struct WebServerConfig {
    std::uint16_t port{8080};
    std::string bindAddress{"0.0.0.0"};
    std::size_t maxViewers{16};
};

// Minimal HTTP/1.1 server that exposes the shared screen to any browser:
//
//   GET /                    viewer page
//   GET /api/streams         JSON list of participants sharing right now
//   GET /stream.mjpeg        multipart/x-mixed-replace live stream
//   GET /snapshot.jpg        latest frame (fallback for browsers without MJPEG)
//   GET /audio.pcm           shared-computer sound as raw PCM16/48 kHz/mono
//
// WSAStartup must already have been called. The server runs one accept thread
// and one thread per connection; slow viewers are disconnected instead of
// blocking the relay loop.
class WebServer {
public:
    explicit WebServer(std::shared_ptr<ScreenStreamHub> hub, std::shared_ptr<SystemAudioHub> audio);
    ~WebServer();

    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    [[nodiscard]] bool Start(const WebServerConfig& config, std::string& error);
    void Stop();

    [[nodiscard]] std::uint16_t Port() const noexcept;
    [[nodiscard]] bool Running() const noexcept;
    [[nodiscard]] std::size_t Viewers() const noexcept;
    [[nodiscard]] std::size_t Listeners() const noexcept;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace vibez::server
