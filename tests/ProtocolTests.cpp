#include "common/protocol/Protocol.hpp"
#include "tests/TestSupport.hpp"

#include <vector>

int main() {
    using namespace vibez::protocol;

    Writer payload;
    CHECK(payload.String("Miguel", 32));
    payload.U16(28765);
    CHECK(payload.String("", 128));
    const ControlMessage original{ControlType::Hello, 42, payload.Data()};
    const auto encoded = EncodeControl(original);
    CHECK(!encoded.empty());

    std::vector<std::uint8_t> fragmented(encoded.begin(), encoded.begin() + 3);
    CHECK(!TryReadControl(fragmented));
    fragmented.insert(fragmented.end(), encoded.begin() + 3, encoded.end());
    const auto decoded = TryReadControl(fragmented);
    CHECK(decoded && decoded->type == ControlType::Hello && decoded->requestId == 42 && decoded->payload == original.payload);
    CHECK(fragmented.empty());

    auto invalid = encoded;
    invalid[0] = 0xFF; invalid[1] = 0xFF; invalid[2] = 0xFF; invalid[3] = 0xFF;
    CHECK(!TryReadControl(invalid));
    CHECK(invalid.empty());

    const MediaHeader audio{MediaType::Audio, 7, 9, 123456, 3};
    auto packet = EncodeMediaHeader(audio);
    CHECK(packet.size() == kMediaHeaderSize);
    packet.insert(packet.end(), {1, 2, 3});
    const auto readAudio = DecodeMediaHeader(packet);
    CHECK(readAudio && readAudio->sessionId == 7 && readAudio->sequence == 9 && readAudio->payloadSize == 3);
    CHECK(readAudio && readAudio->fragmentCount == 1 && readAudio->fragmentIndex == 0 && readAudio->frameId == 0);

    MediaHeader video{MediaType::Video, 4, 100, 9000, 0, kMediaFlagKeyframe, 55, 2, 6};
    const auto videoPacket = EncodeMediaHeader(video);
    CHECK(videoPacket.size() == kMediaHeaderSize);
    const auto readVideo = DecodeMediaHeader(videoPacket);
    CHECK(readVideo && readVideo->type == MediaType::Video && readVideo->frameId == 55);
    CHECK(readVideo && readVideo->fragmentIndex == 2 && readVideo->fragmentCount == 6 && readVideo->IsKeyframe());

    MediaHeader invalidAudio = audio;
    invalidAudio.fragmentCount = 2;
    CHECK(EncodeMediaHeader(invalidAudio).empty());

    MediaHeader invalidVideo = video;
    invalidVideo.fragmentIndex = invalidVideo.fragmentCount;
    CHECK(EncodeMediaHeader(invalidVideo).empty());

    MediaHeader unknownFlags = video;
    unknownFlags.flags = 0x80;
    CHECK(EncodeMediaHeader(unknownFlags).empty());

    auto trulyInvalid = packet;
    trulyInvalid[kMediaHeaderSize] = 4; // Declared payload size remains 3, one byte is missing.
    CHECK(!DecodeMediaHeader(std::span<const std::uint8_t>(trulyInvalid.data(), trulyInvalid.size() - 1)));

    return vibez::test::Finish("Protocol tests");
}
