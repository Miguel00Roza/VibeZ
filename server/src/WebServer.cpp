#include "server/src/WebServer.hpp"

#include "server/src/WebPage.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace vibez::server {
namespace {

constexpr std::size_t kMaxRequestBytes = 8 * 1024;
constexpr std::size_t kMaxTargetLength = 2048;
constexpr int kIoTimeoutMs = 5000;
constexpr auto kFrameWait = std::chrono::milliseconds(500);
constexpr std::string_view kBoundary = "vibezframe";
constexpr std::size_t kDefaultMaxViewers = 16;

struct Request {
    std::string method;
    std::string target;
    bool head{};
};

enum class ReadResult { Ok, Malformed, Closed };

void ConfigureSocket(const SOCKET socket) {
    const DWORD timeout = kIoTimeoutMs;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    const int nodelay = 1;
    setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
}

bool SendAll(const SOCKET socket, const void* data, const std::size_t size) {
    const auto* bytes = static_cast<const char*>(data);
    std::size_t sent{};
    while (sent < size) {
        const int result = send(socket, bytes + sent, static_cast<int>(size - sent), 0);
        if (result <= 0) return false;
        sent += static_cast<std::size_t>(result);
    }
    return true;
}

bool SendText(const SOCKET socket, const std::string_view text) {
    return SendAll(socket, text.data(), text.size());
}

std::string StatusLine(const int code) {
    switch (code) {
    case 200: return "200 OK";
    case 204: return "204 No Content";
    case 400: return "400 Bad Request";
    case 404: return "404 Not Found";
    case 405: return "405 Method Not Allowed";
    case 503: return "503 Service Unavailable";
    default: return "500 Internal Server Error";
    }
}

std::string Response(const int code, const std::string_view contentType, const std::size_t length,
                     const std::string_view extraHeaders = {}) {
    std::string header = "HTTP/1.1 " + StatusLine(code) + "\r\n";
    if (!contentType.empty()) {
        header += "Content-Type: ";
        header.append(contentType);
        header += "\r\n";
    }
    header += "Content-Length: " + std::to_string(length) + "\r\n";
    header += "Cache-Control: no-store, no-cache, must-revalidate\r\n";
    header += "Pragma: no-cache\r\n";
    header += "Connection: close\r\n";
    header += "X-Content-Type-Options: nosniff\r\n";
    header.append(extraHeaders);
    header += "\r\n";
    return header;
}

std::string StreamHeaders() {
    std::string header = "HTTP/1.1 200 OK\r\n";
    header += "Content-Type: multipart/x-mixed-replace; boundary=";
    header.append(kBoundary);
    header += "\r\n";
    header += "Cache-Control: no-store, no-cache, must-revalidate\r\n";
    header += "Pragma: no-cache\r\n";
    header += "Connection: close\r\n";
    header += "X-Accel-Buffering: no\r\n";
    header += "X-Content-Type-Options: nosniff\r\n";
    header += "\r\n";
    return header;
}

// Raw little-endian PCM16, 48 kHz, mono. The browser guards against drift with
// a scheduled buffer; the server only forwards what the sharer encoded.
std::string PcmHeaders() {
    std::string header = "HTTP/1.1 200 OK\r\n";
    header += "Content-Type: audio/L16;rate=48000;channels=1\r\n";
    header += "Cache-Control: no-store, no-cache, must-revalidate\r\n";
    header += "Pragma: no-cache\r\n";
    header += "Connection: close\r\n";
    header += "X-Content-Type-Options: nosniff\r\n";
    header += "\r\n";
    return header;
}

bool IsMethodToken(const std::string_view method) {
    return !method.empty() && method.size() <= 16 &&
           std::all_of(method.begin(), method.end(), [](const char character) { return character >= 'A' && character <= 'Z'; });
}

ReadResult ReadRequest(const SOCKET socket, Request& request) {
    std::string buffer;
    buffer.reserve(1024);
    std::array<char, 1024> chunk{};
    while (true) {
        const int received = recv(socket, chunk.data(), static_cast<int>(chunk.size()), 0);
        if (received <= 0) return ReadResult::Closed;
        buffer.append(chunk.data(), static_cast<std::size_t>(received));
        const auto headerEnd = buffer.find("\r\n\r\n");
        if (headerEnd != std::string::npos) {
            const auto lineEnd = buffer.find("\r\n");
            if (lineEnd == std::string::npos || lineEnd == 0) return ReadResult::Malformed;
            const std::string_view line(buffer.data(), lineEnd);
            const auto firstSpace = line.find(' ');
            const auto secondSpace = firstSpace == std::string_view::npos
                                         ? std::string_view::npos
                                         : line.find(' ', firstSpace + 1);
            if (firstSpace == std::string_view::npos || secondSpace == std::string_view::npos) return ReadResult::Malformed;
            const auto method = line.substr(0, firstSpace);
            const auto target = line.substr(firstSpace + 1, secondSpace - firstSpace - 1);
            const auto version = line.substr(secondSpace + 1);
            if (!IsMethodToken(method)) return ReadResult::Malformed;
            if (version != "HTTP/1.1" && version != "HTTP/1.0") return ReadResult::Malformed;
            // Absolute-form targets are not needed and would only widen the
            // parsing surface of this small server.
            if (target.empty() || target.size() > kMaxTargetLength || target.front() != '/') return ReadResult::Malformed;
            request.method.assign(method);
            request.target.assign(target);
            request.head = method == "HEAD";
            return ReadResult::Ok;
        }
        if (buffer.size() > kMaxRequestBytes) return ReadResult::Malformed;
    }
}

std::optional<std::uint32_t> QuerySession(const std::string_view query) {
    const auto position = query.find("session=");
    if (position == std::string_view::npos) return std::nullopt;
    auto value = query.substr(position + 8);
    if (const auto end = value.find('&'); end != std::string_view::npos) value = value.substr(0, end);
    if (value.empty()) return std::nullopt;
    std::uint32_t session{};
    const auto [pointer, error] = std::from_chars(value.data(), value.data() + value.size(), session);
    if (error != std::errc{} || pointer != value.data() + value.size()) return std::nullopt;
    return session;
}

std::string JsonEscape(const std::string_view text) {
    std::string result;
    result.reserve(text.size() + 8);
    for (const unsigned char character : text) {
        switch (character) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (character < 0x20) {
                char buffer[8]{};
                std::snprintf(buffer, sizeof(buffer), "\\u%04X", character);
                result += buffer;
            } else {
                result += static_cast<char>(character);
            }
        }
    }
    return result;
}

std::string StreamsJson(const std::vector<StreamInfo>& streams, const std::size_t viewers,
                        const std::size_t listeners, const SystemAudioHub& audio) {
    std::string json = "{\"viewers\":" + std::to_string(viewers) + ",\"listeners\":" + std::to_string(listeners) +
                       ",\"streams\":[";
    bool first = true;
    for (const auto& stream : streams) {
        if (!first) json += ',';
        first = false;
        char fps[32]{};
        std::snprintf(fps, sizeof(fps), "%.1f", stream.fps);
        json += "{\"id\":" + std::to_string(stream.id);
        json += ",\"name\":\"" + JsonEscape(stream.name) + "\"";
        json += ",\"channelId\":" + std::to_string(stream.channelId);
        json += ",\"channel\":\"" + JsonEscape(stream.channel) + "\"";
        json += ",\"width\":" + std::to_string(stream.width);
        json += ",\"height\":" + std::to_string(stream.height);
        json += ",\"fps\":" + std::string(fps);
        json += ",\"audio\":";
        json += audio.HasRecentAudio(stream.id) ? "true" : "false";
        json += "}";
    }
    json += "]}";
    return json;
}

bool PeerClosed(const SOCKET socket) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(socket, &readable);
    timeval timeout{0, 0};
    if (select(0, &readable, nullptr, nullptr, &timeout) <= 0) return false;
    char byte{};
    const int received = recv(socket, &byte, 1, MSG_PEEK);
    if (received == 0) return true;
    if (received < 0) {
        const int error = WSAGetLastError();
        return error == WSAECONNRESET || error == WSAECONNABORTED || error == WSAENOTCONN;
    }
    return false;
}

} // namespace

struct WebServer::Impl {
    std::shared_ptr<ScreenStreamHub> hub;
    std::shared_ptr<SystemAudioHub> audio;
    std::atomic<SOCKET> listener{INVALID_SOCKET};
    std::atomic<std::uint16_t> port{0};
    std::atomic_bool stopping{false};
    std::atomic<int> viewers{0};
    std::atomic<int> listeners{0};
    std::atomic<int> activeConnections{0};
    std::size_t maxViewers{kDefaultMaxViewers};
    std::thread acceptThread;
    std::mutex connectionsMutex;
    std::condition_variable idle;
    std::unordered_set<SOCKET> connections;

    void AcceptLoop(const std::shared_ptr<Impl>& self) {
        while (!stopping.load()) {
            const SOCKET server = listener.load();
            if (server == INVALID_SOCKET) break;
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(server, &readable);
            timeval timeout{0, 200000};
            if (select(0, &readable, nullptr, nullptr, &timeout) <= 0) continue;

            sockaddr_in remote{};
            int size = sizeof(remote);
            const SOCKET client = accept(server, reinterpret_cast<sockaddr*>(&remote), &size);
            if (client == INVALID_SOCKET) continue;
            {
                std::lock_guard lock(connectionsMutex);
                connections.insert(client);
            }
            activeConnections.fetch_add(1);
            std::thread([self, client] { self->HandleConnection(self, client); }).detach();
        }
    }

    void Finish(const SOCKET socket) {
        closesocket(socket);
        {
            std::lock_guard lock(connectionsMutex);
            connections.erase(socket);
        }
        activeConnections.fetch_sub(1);
        idle.notify_all();
    }

    void HandleConnection(const std::shared_ptr<Impl>& self, const SOCKET socket) {
        ConfigureSocket(socket);
        Request request;
        const auto result = ReadRequest(socket, request);
        if (result == ReadResult::Ok) {
            Route(socket, request);
        } else if (result == ReadResult::Malformed) {
            const std::string body = "Bad request.";
            SendText(socket, Response(400, "text/plain; charset=utf-8", body.size()));
            SendText(socket, body);
        }
        self->Finish(socket);
    }

    void Route(const SOCKET socket, const Request& request) {
        if (request.method != "GET" && request.method != "HEAD") {
            const std::string body = "Method not allowed.";
            SendText(socket, Response(405, "text/plain; charset=utf-8", body.size(), "Allow: GET, HEAD\r\n"));
            if (!request.head) SendText(socket, body);
            return;
        }

        const auto queryPosition = request.target.find('?');
        const std::string_view target(request.target);
        const std::string_view path = target.substr(0, queryPosition);
        const std::string_view query = queryPosition == std::string::npos
                                           ? std::string_view{}
                                           : target.substr(queryPosition + 1);
        const std::uint32_t session = QuerySession(query).value_or(0);

        if (path == "/" || path == "/index.html") {
            SendText(socket, Response(200, "text/html; charset=utf-8", kWebPageHtml.size()));
            if (!request.head) SendText(socket, kWebPageHtml);
            return;
        }
        if (path == "/api/streams") {
            const auto json = StreamsJson(hub->Streams(), static_cast<std::size_t>(viewers.load()),
                                          static_cast<std::size_t>(listeners.load()), *audio);
            SendText(socket, Response(200, "application/json; charset=utf-8", json.size()));
            if (!request.head) SendText(socket, json);
            return;
        }
        if (path == "/snapshot.jpg") {
            const auto frame = hub->LatestFrame(session);
            if (!frame || !frame->jpeg || frame->jpeg->empty()) {
                const std::string body = "No screen is being shared right now.";
                SendText(socket, Response(503, "text/plain; charset=utf-8", body.size()));
                if (!request.head) SendText(socket, body);
                return;
            }
            SendText(socket, Response(200, "image/jpeg", frame->jpeg->size()));
            if (!request.head) SendAll(socket, frame->jpeg->data(), frame->jpeg->size());
            return;
        }
        if (path == "/stream.mjpeg") {
            SendStream(socket, session, request.head);
            return;
        }
        if (path == "/audio.pcm") {
            SendAudio(socket, session, request.head);
            return;
        }
        if (path == "/favicon.ico") {
            SendText(socket, Response(204, {}, 0));
            return;
        }

        const std::string body = "<!doctype html><meta charset=\"utf-8\"><title>VibeZ</title>"
                                 "<p>Not found. Open <a href=\"/\">the viewer</a>.</p>";
        SendText(socket, Response(404, "text/html; charset=utf-8", body.size()));
        if (!request.head) SendText(socket, body);
    }

    void SendStream(const SOCKET socket, const std::uint32_t session, const bool head) {
        int current = viewers.load();
        while (current < static_cast<int>(maxViewers) &&
               !viewers.compare_exchange_weak(current, current + 1)) {
        }
        if (current >= static_cast<int>(maxViewers)) {
            const std::string body = "Too many viewers. Try again in a moment.";
            SendText(socket, Response(503, "text/plain; charset=utf-8", body.size()));
            if (!head) SendText(socket, body);
            return;
        }

        SendText(socket, StreamHeaders());
        if (head) {
            viewers.fetch_sub(1);
            return;
        }
        struct ViewerGuard {
            std::atomic<int>& viewers;
            ~ViewerGuard() { viewers.fetch_sub(1); }
        } guard{viewers};

        if (!SendText(socket, "--" + std::string(kBoundary) + "\r\n")) return;

        std::uint64_t lastGeneration = 0;
        while (!stopping.load()) {
            const auto frame = hub->WaitForFrame(session, lastGeneration, kFrameWait);
            if (stopping.load()) break;
            if (!frame) {
                // No frame within the timeout: drop the connection when the
                // browser went away, otherwise keep waiting for a stream.
                if (PeerClosed(socket)) break;
                continue;
            }
            lastGeneration = frame->generation;
            if (!frame->jpeg || frame->jpeg->empty()) continue;
            const auto partHeader = "Content-Type: image/jpeg\r\nContent-Length: " +
                                    std::to_string(frame->jpeg->size()) + "\r\n\r\n";
            if (!SendText(socket, partHeader)) break;
            if (!SendAll(socket, frame->jpeg->data(), frame->jpeg->size())) break;
            if (!SendText(socket, "\r\n--" + std::string(kBoundary) + "\r\n")) break;
        }
    }

    // Streams the shared-computer sound as raw PCM16. Audio connections do not
    // touch video frames, so a browser can listen even while the screen is
    // paused.
    void SendAudio(const SOCKET socket, const std::uint32_t session, const bool head) {
        int current = listeners.load();
        while (current < static_cast<int>(maxViewers) &&
               !listeners.compare_exchange_weak(current, current + 1)) {
        }
        if (current >= static_cast<int>(maxViewers)) {
            const std::string body = "Too many listeners. Try again in a moment.";
            SendText(socket, Response(503, "text/plain; charset=utf-8", body.size()));
            if (!head) SendText(socket, body);
            return;
        }
        if (audio->Resolve(session) == 0) {
            listeners.fetch_sub(1);
            const std::string body = "No shared audio is available right now.";
            SendText(socket, Response(503, "text/plain; charset=utf-8", body.size()));
            if (!head) SendText(socket, body);
            return;
        }

        SendText(socket, PcmHeaders());
        if (head) {
            listeners.fetch_sub(1);
            return;
        }
        struct ListenerGuard {
            std::atomic<int>& listeners;
            ~ListenerGuard() { listeners.fetch_sub(1); }
        } guard{listeners};

        std::uint64_t cursor = SystemAudioHub::kLiveCursor;
        std::vector<std::int16_t> samples;
        while (!stopping.load()) {
            samples.clear();
            if (!audio->Read(session, cursor, samples, 4800, kFrameWait)) {
                if (stopping.load()) break;
                // The share ended or nobody is sharing yet: let the page
                // reconnect and show its waiting state.
                if (audio->Resolve(session) == 0) break;
                if (PeerClosed(socket)) break;
                continue;
            }
            if (samples.empty()) continue;
            if (!SendAll(socket, samples.data(), samples.size() * sizeof(std::int16_t))) break;
        }
    }
};

WebServer::WebServer(std::shared_ptr<ScreenStreamHub> hub, std::shared_ptr<SystemAudioHub> audio)
    : impl_(std::make_shared<Impl>()) {
    impl_->hub = std::move(hub);
    impl_->audio = std::move(audio);
}

WebServer::~WebServer() { Stop(); }

bool WebServer::Start(const WebServerConfig& config, std::string& error) {
    if (!impl_ || !impl_->hub || !impl_->audio) {
        error = "The web server has no media hub";
        return false;
    }
    impl_->maxViewers = config.maxViewers == 0 ? kDefaultMaxViewers : config.maxViewers;
    impl_->stopping.store(false);

    const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        error = "Cannot create the web socket (error " + std::to_string(WSAGetLastError()) + ")";
        return false;
    }
    const BOOL reuse = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(config.port);
    if (InetPtonA(AF_INET, config.bindAddress.c_str(), &address.sin_addr) != 1) {
        address.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        listen(listener, SOMAXCONN) == SOCKET_ERROR) {
        const auto code = WSAGetLastError();
        closesocket(listener);
        error = "Cannot bind the web server to " + config.bindAddress + ":" + std::to_string(config.port) +
                " (error " + std::to_string(code) + ")";
        return false;
    }

    sockaddr_in bound{};
    int size = sizeof(bound);
    impl_->port.store(getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &size) == 0
                          ? ntohs(bound.sin_port)
                          : config.port);
    impl_->listener.store(listener);

    const std::shared_ptr<Impl> self = impl_;
    impl_->acceptThread = std::thread([self] { self->AcceptLoop(self); });
    return true;
}

void WebServer::Stop() {
    if (!impl_ || impl_->stopping.exchange(true)) return;

    if (const SOCKET listener = impl_->listener.exchange(INVALID_SOCKET); listener != INVALID_SOCKET) {
        closesocket(listener);
    }
    {
        // Shutting the connection sockets down (instead of closing them here)
        // makes recv/send fail in the connection threads, which then close the
        // sockets themselves and cannot race with this function.
        std::lock_guard lock(impl_->connectionsMutex);
        for (const SOCKET socket : impl_->connections) shutdown(socket, SD_BOTH);
    }
    if (impl_->acceptThread.joinable()) impl_->acceptThread.join();

    std::unique_lock lock(impl_->connectionsMutex);
    impl_->idle.wait_for(lock, std::chrono::seconds(3), [this] { return impl_->activeConnections.load() == 0; });
    impl_->port.store(0);
}

std::uint16_t WebServer::Port() const noexcept { return impl_ ? impl_->port.load() : 0; }
bool WebServer::Running() const noexcept { return impl_ && impl_->listener.load() != INVALID_SOCKET; }
std::size_t WebServer::Viewers() const noexcept { return impl_ ? static_cast<std::size_t>(impl_->viewers.load()) : 0; }
std::size_t WebServer::Listeners() const noexcept { return impl_ ? static_cast<std::size_t>(impl_->listeners.load()) : 0; }

} // namespace vibez::server
