#include "common/protocol/Protocol.hpp"
#include "tests/TestSupport.hpp"

#include <vector>

int main() {
    using namespace vibez::protocol;

    CHECK(kProtocolVersion == 4);
    CHECK(kMediaFlagSystemAudio == 0x02);

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

    // The microphone stream never carries a keyframe flag; the shared computer
    // sound is identified only by kMediaFlagSystemAudio.
    MediaHeader systemAudio = audio;
    systemAudio.flags = kMediaFlagSystemAudio;
    auto systemPacket = EncodeMediaHeader(systemAudio);
    CHECK(systemPacket.size() == kMediaHeaderSize);
    systemPacket.insert(systemPacket.end(), {1, 2, 3});
    const auto readSystem = DecodeMediaHeader(systemPacket);
    CHECK(readSystem && (readSystem->flags & kMediaFlagSystemAudio) != 0);
    MediaHeader audioKeyframe = audio;
    audioKeyframe.flags = kMediaFlagKeyframe;
    CHECK(EncodeMediaHeader(audioKeyframe).empty());
    MediaHeader audioUnknownFlag = audio;
    audioUnknownFlag.flags = 0x80;
    CHECK(EncodeMediaHeader(audioUnknownFlag).empty());

    // Keepalive datagrams carry no payload and exist only to keep the NAT
    // mapping and the server-side endpoint learning alive.
    const MediaHeader keepalive{MediaType::Keepalive, 7, 0, 999, 0};
    const auto keepalivePacket = EncodeMediaHeader(keepalive);
    CHECK(keepalivePacket.size() == kMediaHeaderSize);
    const auto readKeepalive = DecodeMediaHeader(keepalivePacket);
    CHECK(readKeepalive && readKeepalive->type == MediaType::Keepalive && readKeepalive->payloadSize == 0);
    MediaHeader invalidKeepalive = keepalive;
    invalidKeepalive.payloadSize = 1;
    CHECK(EncodeMediaHeader(invalidKeepalive).empty());

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
