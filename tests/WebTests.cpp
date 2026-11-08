#include "common/protocol/Protocol.hpp"
#include "server/include/vibez/server/Server.hpp"
#include "server/src/ScreenStream.hpp"
#include "server/src/WebServer.hpp"
#include "tests/TestSupport.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using vibez::protocol::ControlType;
using vibez::protocol::MediaType;
using vibez::server::ScreenStreamHub;
using vibez::server::SessionInfo;
using vibez::server::WebServer;
using vibez::server::WebServerConfig;

std::uint16_t BoundPort(const SOCKET socket) {
    sockaddr_in address{};
    int size = sizeof(address);
    return getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size) == 0 ? ntohs(address.sin_port) : 0;
}

std::uint16_t FindFreePort() {
    const SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) return 0;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        closesocket(socket);
        return 0;
    }
    const auto port = BoundPort(socket);
    closesocket(socket);
    return port;
}

std::vector<std::uint8_t> FakeJpeg(const std::size_t size, const std::uint8_t seed) {
    std::vector<std::uint8_t> data(size);
    for (std::size_t index = 0; index < data.size(); ++index) {
        data[index] = static_cast<std::uint8_t>(seed + index * 7);
    }
    if (data.size() >= 2) {
        data[0] = 0xFF;
        data[1] = 0xD8;
    }
    return data;
}

std::vector<std::vector<std::uint8_t>> Split(const std::vector<std::uint8_t>& data, const std::size_t chunk) {
    std::vector<std::vector<std::uint8_t>> parts;
    for (std::size_t offset = 0; offset < data.size(); offset += chunk) {
        const auto end = (std::min)(data.size(), offset + chunk);
        parts.emplace_back(data.begin() + static_cast<std::ptrdiff_t>(offset),
                           data.begin() + static_cast<std::ptrdiff_t>(end));
    }
    return parts;
}

void PushFrame(ScreenStreamHub& hub, const std::uint32_t session, const std::uint32_t frameId, const bool keyframe,
               const std::vector<std::uint8_t>& jpeg, const std::size_t chunk = 32,
               const std::vector<std::size_t>* order = nullptr) {
    const auto parts = Split(jpeg, chunk);
    std::vector<std::size_t> indices(parts.size());
    for (std::size_t index = 0; index < parts.size(); ++index) indices[index] = index;
    if (order != nullptr) indices = *order;

    for (const auto index : indices) {
        if (index >= parts.size()) continue;
        vibez::protocol::MediaHeader header;
        header.type = MediaType::Video;
        header.sessionId = session;
        header.frameId = frameId;
        header.fragmentIndex = static_cast<std::uint16_t>(index);
        header.fragmentCount = static_cast<std::uint16_t>(parts.size());
        header.payloadSize = static_cast<std::uint16_t>(parts[index].size());
        header.flags = keyframe ? vibez::protocol::kMediaFlagKeyframe : 0;
        hub.PushFragment(header, parts[index]);
    }
}

SOCKET ConnectTo(const std::uint16_t port) {
    const SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) return INVALID_SOCKET;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        closesocket(socket);
        return INVALID_SOCKET;
    }
    const DWORD timeout = 5000;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    return socket;
}

bool SendBytes(const SOCKET socket, const void* data, const std::size_t size) {
    const auto* bytes = static_cast<const char*>(data);
    std::size_t sent{};
    while (sent < size) {
        const int result = send(socket, bytes + sent, static_cast<int>(size - sent), 0);
        if (result <= 0) return false;
        sent += static_cast<std::size_t>(result);
    }
    return true;
}

std::string ReadUntilClosed(const SOCKET socket, const std::size_t limit = 1 << 20) {
    std::string data;
    std::array<char, 4096> buffer{};
    while (data.size() < limit) {
        const int received = recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (received <= 0) break;
        data.append(buffer.data(), static_cast<std::size_t>(received));
    }
    return data;
}

std::string ReadUntil(const SOCKET socket, const std::string_view needle, const std::size_t limit = 1 << 20) {
    std::string data;
    std::array<char, 4096> buffer{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (data.size() < limit && std::chrono::steady_clock::now() < deadline) {
        const int received = recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (received <= 0) break;
        data.append(buffer.data(), static_cast<std::size_t>(received));
        if (data.find(needle) != std::string::npos) break;
    }
    return data;
}

int StatusCode(const std::string& response) {
    return response.size() >= 12 ? std::atoi(response.substr(9, 3).c_str()) : 0;
}

std::string_view BodyOf(const std::string& response) {
    const auto end = response.find("\r\n\r\n");
    if (end == std::string::npos) return {};
    return std::string_view(response.data() + end + 4, response.size() - end - 4);
}

std::string HttpGet(const std::uint16_t port, const std::string& target) {
    const SOCKET socket = ConnectTo(port);
    if (socket == INVALID_SOCKET) return {};
    const std::string request = "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    std::string response;
    if (SendBytes(socket, request.data(), request.size())) response = ReadUntilClosed(socket);
    closesocket(socket);
    return response;
}

std::string RawRequest(const std::uint16_t port, const std::string& request) {
    const SOCKET socket = ConnectTo(port);
    if (socket == INVALID_SOCKET) return {};
    std::string response;
    if (SendBytes(socket, request.data(), request.size())) response = ReadUntilClosed(socket);
    closesocket(socket);
    return response;
}

void TestReassembly() {
    ScreenStreamHub hub;
    hub.SyncSessions({{1, "Alice", 2, "Amigos", true, 640, 480}});
    CHECK(hub.Streams().size() == 1);
    CHECK(hub.Streams().front().name == "Alice");
    CHECK(hub.Streams().front().channel == "Amigos");

    const auto jpeg = FakeJpeg(100, 11);
    // Seven fragments delivered out of order must still reassemble.
    const std::vector<std::size_t> order{6, 2, 0, 5, 1, 4, 3};
    PushFrame(hub, 1, 7, true, jpeg, 16, &order);
    auto frame = hub.LatestFrame(1);
    CHECK_MSG(frame.has_value(), "Out-of-order fragments were not reassembled");
    if (frame) {
        CHECK(frame->generation > 0);
        CHECK(frame->keyframe);
        CHECK(frame->jpeg && *frame->jpeg == jpeg);
    }

    // Duplicated datagrams must not corrupt the frame or double count.
    const std::vector<std::size_t> withDuplicate{0, 0, 1, 2, 3, 4, 5};
    const auto second = FakeJpeg(90, 21);
    PushFrame(hub, 1, 9, false, second, 16, &withDuplicate);
    frame = hub.LatestFrame(1);
    CHECK(frame && frame->jpeg && *frame->jpeg == second);

    // An incomplete frame is never published.
    const auto generation = frame ? frame->generation : 0;
    const auto parts = Split(second, 16);
    vibez::protocol::MediaHeader partial;
    partial.type = MediaType::Video;
    partial.sessionId = 1;
    partial.frameId = 20;
    partial.fragmentCount = static_cast<std::uint16_t>(parts.size());
    partial.payloadSize = static_cast<std::uint16_t>(parts.front().size());
    hub.PushFragment(partial, parts.front());
    frame = hub.LatestFrame(1);
    CHECK(frame && frame->generation == generation);

    // A datagram bigger than the frame budget is dropped instead of buffered.
    ScreenStreamHub budget;
    budget.SyncSessions({{3, "Bob", 2, "Amigos", true, 640, 480}});
    vibez::protocol::MediaHeader huge;
    huge.type = MediaType::Video;
    huge.sessionId = 3;
    huge.frameId = 1;
    huge.fragmentCount = 1;
    const std::vector<std::uint8_t> oversized(vibez::protocol::kMaxVideoFrameBytes + 1, 0x42);
    budget.PushFragment(huge, oversized);
    CHECK(!budget.LatestFrame(3).has_value());

    // A peer cannot change the fragment count of an in-flight frame; doing so
    // used to index outside the received map.
    ScreenStreamHub hostile;
    hostile.SyncSessions({{5, "Mallory", 2, "Amigos", true, 640, 480}});
    vibez::protocol::MediaHeader wide;
    wide.type = MediaType::Video;
    wide.sessionId = 5;
    wide.frameId = 1;
    wide.fragmentIndex = 0;
    wide.fragmentCount = 8;
    const std::vector<std::uint8_t> firstPartData{1, 2, 3, 4};
    hostile.PushFragment(wide, firstPartData);
    auto narrow = wide;
    narrow.fragmentCount = 2;
    narrow.fragmentIndex = 1;
    const std::vector<std::uint8_t> secondPartData{5, 6, 7, 8};
    hostile.PushFragment(narrow, secondPartData);
    CHECK(!hostile.LatestFrame(5).has_value());

    // Sessions that are not sharing never publish frames.
    ScreenStreamHub muted;
    muted.SyncSessions({{4, "Carol", 2, "Amigos", false, 0, 0}});
    PushFrame(muted, 4, 1, true, jpeg, 16);
    CHECK(!muted.LatestFrame(4).has_value());
    CHECK(muted.Streams().empty());

    // Stopping a share drops the last frame; disconnect forgets the session.
    hub.SyncSessions({{1, "Alice", 2, "Amigos", false, 0, 0}});
    CHECK(!hub.LatestFrame(1).has_value());
    CHECK(hub.Streams().empty());
    hub.SyncSessions({});
    CHECK(hub.Streams().empty());
}

void TestWaitingAndAutofollow() {
    ScreenStreamHub hub;
    hub.SyncSessions({{1, "Alice", 2, "Amigos", true, 640, 480}});
    const auto first = FakeJpeg(64, 1);
    PushFrame(hub, 1, 1, true, first, 16);

    const auto immediate = hub.WaitForFrame(1, 0, std::chrono::milliseconds(50));
    CHECK(immediate && immediate->jpeg && *immediate->jpeg == first);
    const auto generation = immediate ? immediate->generation : 0;
    CHECK(!hub.WaitForFrame(1, generation, std::chrono::milliseconds(60)).has_value());
    CHECK(hub.NewestSession() == 1);

    // A second sharer becomes the newest one; auto-follow (session 0) follows.
    hub.SyncSessions({{1, "Alice", 2, "Amigos", true, 640, 480},
                      {2, "Bob", 2, "Amigos", true, 800, 600}});
    const auto second = FakeJpeg(70, 9);
    PushFrame(hub, 2, 1, true, second, 16);
    CHECK(hub.NewestSession() == 2);
    const auto followed = hub.WaitForFrame(0, 0, std::chrono::milliseconds(100));
    CHECK(followed && followed->sessionId == 2 && followed->jpeg && *followed->jpeg == second);
    CHECK(hub.Streams().front().id == 2);

    // Per-session isolation: Bob's frame must not leak into Alice's stream.
    CHECK(hub.LatestFrame(1) && *hub.LatestFrame(1)->jpeg == first);
    CHECK(!hub.LatestFrame(99).has_value());
}

void TestHttpServer() {
    auto hub = std::make_shared<ScreenStreamHub>();
    hub->SyncSessions({{1, "Alice", 2, "Amigos", true, 640, 480}});
    const auto jpeg = FakeJpeg(3000, 5);
    PushFrame(*hub, 1, 1, true, jpeg, 600);

    WebServer web(hub);
    std::string error;
    WebServerConfig config;
    config.port = 0;
    config.bindAddress = "127.0.0.1";
    config.maxViewers = 4;
    CHECK_MSG(web.Start(config, error), error);
    const auto port = web.Port();
    CHECK(port != 0);
    CHECK(web.Running());

    const auto page = HttpGet(port, "/");
    CHECK(StatusCode(page) == 200);
    CHECK(page.find("text/html") != std::string::npos);
    CHECK(page.find("VibeZ") != std::string::npos);
    CHECK(page.find("stream.mjpeg") != std::string::npos);

    const auto head = RawRequest(port, "HEAD / HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(StatusCode(head) == 200);
    CHECK(BodyOf(head).empty());

    const auto api = HttpGet(port, "/api/streams");
    CHECK(StatusCode(api) == 200);
    CHECK(api.find("\"name\":\"Alice\"") != std::string::npos);
    CHECK(api.find("\"channel\":\"Amigos\"") != std::string::npos);
    CHECK(api.find("\"fps\":") != std::string::npos);

    for (const std::string target : {std::string("/snapshot.jpg"), std::string("/snapshot.jpg?session=1")}) {
        const auto snapshot = HttpGet(port, target);
        CHECK(StatusCode(snapshot) == 200);
        CHECK(snapshot.find("Content-Type: image/jpeg") != std::string::npos);
        const auto body = BodyOf(snapshot);
        CHECK(body.size() == jpeg.size());
        CHECK(std::memcmp(body.data(), jpeg.data(), jpeg.size()) == 0);
    }

    CHECK(StatusCode(HttpGet(port, "/snapshot.jpg?session=42")) == 503);
    CHECK(StatusCode(HttpGet(port, "/missing")) == 404);
    CHECK(StatusCode(RawRequest(port, "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n")) == 405);
    CHECK(StatusCode(RawRequest(port, "garbage\r\n\r\n")) == 400);

    // Live stream: headers, first part, then a new frame delivered as a second part.
    const SOCKET stream = ConnectTo(port);
    CHECK(stream != INVALID_SOCKET);
    if (stream != INVALID_SOCKET) {
        const std::string request = "GET /stream.mjpeg?session=1 HTTP/1.1\r\nHost: x\r\n\r\n";
        CHECK(SendBytes(stream, request.data(), request.size()));
        const auto firstPart = ReadUntil(stream, "Content-Length: 3000", 64 * 1024);
        CHECK(firstPart.find("multipart/x-mixed-replace") != std::string::npos);
        CHECK(firstPart.find("boundary=vibezframe") != std::string::npos);
        CHECK(firstPart.find("Content-Type: image/jpeg") != std::string::npos);

        const auto next = FakeJpeg(2800, 77);
        PushFrame(*hub, 1, 2, false, next, 600);
        const auto secondPart = ReadUntil(stream, "Content-Length: 2800", 64 * 1024);
        CHECK(secondPart.find("Content-Length: 2800") != std::string::npos);
        closesocket(stream);
    }

    // Wait for the viewer slot to be released before shutting down.
    for (int attempt = 0; attempt < 100 && web.Viewers() != 0; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(web.Viewers() == 0);

    web.Stop();
    CHECK(!web.Running());
    CHECK(web.Port() == 0);
}

void TestViewerLimit() {
    auto hub = std::make_shared<ScreenStreamHub>();
    WebServer web(hub);
    std::string error;
    WebServerConfig config;
    config.port = 0;
    config.bindAddress = "127.0.0.1";
    config.maxViewers = 1;
    CHECK_MSG(web.Start(config, error), error);
    const auto port = web.Port();

    const SOCKET first = ConnectTo(port);
    CHECK(first != INVALID_SOCKET);
    if (first != INVALID_SOCKET) {
        const std::string request = "GET /stream.mjpeg?session=0 HTTP/1.1\r\nHost: x\r\n\r\n";
        CHECK(SendBytes(first, request.data(), request.size()));
        const auto headers = ReadUntil(first, "multipart/x-mixed-replace", 4096);
        CHECK(headers.find("multipart/x-mixed-replace") != std::string::npos);
        for (int attempt = 0; attempt < 50 && web.Viewers() != 1; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CHECK(web.Viewers() == 1);
        CHECK(StatusCode(HttpGet(port, "/stream.mjpeg")) == 503);
        closesocket(first);
    }
    web.Stop();
}

bool SendVideoFragments(const SOCKET udp, const std::uint16_t port, const std::uint32_t session,
                        const std::vector<std::uint8_t>& jpeg) {
    constexpr std::size_t kChunk = 1000;
    const auto count = (jpeg.size() + kChunk - 1) / kChunk;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    for (std::size_t index = 0; index < count; ++index) {
        const auto offset = index * kChunk;
        const auto bytes = (std::min)(kChunk, jpeg.size() - offset);
        vibez::protocol::MediaHeader header;
        header.type = MediaType::Video;
        header.sessionId = session;
        header.sequence = static_cast<std::uint32_t>(index);
        header.timestamp = 1;
        header.payloadSize = static_cast<std::uint16_t>(bytes);
        header.flags = vibez::protocol::kMediaFlagKeyframe;
        header.frameId = 1;
        header.fragmentIndex = static_cast<std::uint16_t>(index);
        header.fragmentCount = static_cast<std::uint16_t>(count);
        const auto headerBytes = vibez::protocol::EncodeMediaHeader(header);
        if (headerBytes.empty()) return false;
        std::vector<std::uint8_t> packet = headerBytes;
        packet.insert(packet.end(), jpeg.begin() + static_cast<std::ptrdiff_t>(offset),
                      jpeg.begin() + static_cast<std::ptrdiff_t>(offset + bytes));
        if (sendto(udp, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
                   reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != static_cast<int>(packet.size())) {
            return false;
        }
    }
    return true;
}

std::optional<vibez::protocol::ControlMessage> ReadControl(const SOCKET socket, std::vector<std::uint8_t>& inbox,
                                                          const int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::array<std::uint8_t, 4096> buffer{};
    while (std::chrono::steady_clock::now() < deadline) {
        if (const auto message = vibez::protocol::TryReadControl(inbox)) return message;
        const int received = recv(socket, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
        if (received <= 0) break;
        inbox.insert(inbox.end(), buffer.begin(), buffer.begin() + received);
    }
    return vibez::protocol::TryReadControl(inbox);
}

// Full path: a real client shares through the real server, and an HTTP request
// retrieves exactly the JPEG that travelled over UDP.
void TestEndToEnd() {
    const auto mediaPort = FindFreePort();
    CHECK(mediaPort != 0);
    if (mediaPort == 0) return;

    vibez::server::Server server(mediaPort);
    vibez::server::WebConfig web;
    web.port = 0; // Ephemeral port, discovered through Server::WebPort().
    web.bindAddress = "127.0.0.1";
    server.SetWebConfig(web);
    std::thread serverThread([&] { (void)server.Run(); });

    SOCKET control = INVALID_SOCKET;
    for (int attempt = 0; attempt < 100 && control == INVALID_SOCKET; ++attempt) {
        control = ConnectTo(mediaPort);
        if (control == INVALID_SOCKET) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK_MSG(control != INVALID_SOCKET, "Could not connect to the test server");
    if (control == INVALID_SOCKET) {
        server.RequestStop();
        serverThread.join();
        return;
    }

    vibez::protocol::Writer hello;
    hello.String("Alice", 32);
    hello.U16(40000);
    hello.String("", 128);
    const auto helloPacket = vibez::protocol::EncodeControl({ControlType::Hello, 1, hello.Data()});
    CHECK(!helloPacket.empty());
    CHECK(SendBytes(control, helloPacket.data(), helloPacket.size()));

    std::vector<std::uint8_t> inbox;
    std::uint32_t sessionId = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (sessionId == 0 && std::chrono::steady_clock::now() < deadline) {
        const auto message = ReadControl(control, inbox, 1000);
        if (!message) continue;
        if (message->type == ControlType::HelloAccepted) {
            vibez::protocol::Reader reader(message->payload);
            std::uint32_t channel{};
            if (!reader.U32(sessionId) || !reader.U32(channel)) sessionId = 0;
        }
    }
    CHECK_MSG(sessionId != 0, "The server did not accept the test client");

    vibez::protocol::Writer sharing;
    sharing.U8(1);
    sharing.U16(640);
    sharing.U16(480);
    const auto sharingPacket = vibez::protocol::EncodeControl({ControlType::SetSharing, 2, sharing.Data()});
    CHECK(SendBytes(control, sharingPacket.data(), sharingPacket.size()));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    const SOCKET udp = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    CHECK(udp != INVALID_SOCKET);
    const auto jpeg = FakeJpeg(2400, 31);
    CHECK(SendVideoFragments(udp, mediaPort, sessionId, jpeg));

    std::uint16_t webPort = 0;
    for (int attempt = 0; attempt < 100 && webPort == 0; ++attempt) {
        webPort = server.WebPort();
        if (webPort == 0) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK_MSG(webPort != 0, "The web viewer did not start");

    std::string snapshot;
    for (int attempt = 0; attempt < 100 && webPort != 0; ++attempt) {
        snapshot = HttpGet(webPort, "/snapshot.jpg?session=" + std::to_string(sessionId));
        if (StatusCode(snapshot) == 200) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK_MSG(StatusCode(snapshot) == 200, "The web viewer never received the shared frame");
    if (StatusCode(snapshot) == 200) {
        const auto body = BodyOf(snapshot);
        CHECK(body.size() == jpeg.size());
        CHECK(std::memcmp(body.data(), jpeg.data(), jpeg.size()) == 0);
    }

    const auto api = HttpGet(webPort, "/api/streams");
    CHECK(StatusCode(api) == 200);
    CHECK(api.find("\"name\":\"Alice\"") != std::string::npos);

    // Stopping the share must remove the stream from the page immediately.
    vibez::protocol::Writer stopped;
    stopped.U8(0);
    stopped.U16(0);
    stopped.U16(0);
    const auto stoppedPacket = vibez::protocol::EncodeControl({ControlType::SetSharing, 3, stopped.Data()});
    CHECK(SendBytes(control, stoppedPacket.data(), stoppedPacket.size()));
    std::string cleared;
    for (int attempt = 0; attempt < 60; ++attempt) {
        cleared = HttpGet(webPort, "/api/streams");
        if (cleared.find("\"streams\":[]") != std::string::npos) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(cleared.find("\"streams\":[]") != std::string::npos);

    closesocket(control);
    closesocket(udp);
    server.RequestStop();
    serverThread.join();
    CHECK(server.WebPort() == 0);
}

} // namespace

int main() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;

    TestReassembly();
    TestWaitingAndAutofollow();
    TestHttpServer();
    TestViewerLimit();
    TestEndToEnd();

    WSACleanup();
    return vibez::test::Finish("Web tests");
}
