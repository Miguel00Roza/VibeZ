#include "common/protocol/Protocol.hpp"

namespace vibez::protocol {
namespace {
constexpr std::size_t kControlHeaderSize = 4 + 2 + 1 + 4 + 4;

bool IsControlType(const std::uint8_t value) {
    return value >= static_cast<std::uint8_t>(ControlType::Hello) && value <= static_cast<std::uint8_t>(ControlType::ChatMessage);
}
bool IsMediaType(const std::uint8_t value) {
    return value == static_cast<std::uint8_t>(MediaType::Audio) ||
           value == static_cast<std::uint8_t>(MediaType::Video) ||
           value == static_cast<std::uint8_t>(MediaType::Keepalive);
}
bool IsValidMediaHeader(const MediaHeader& header) {
    if (header.payloadSize > kMaxMediaPayload || !IsMediaType(static_cast<std::uint8_t>(header.type))) return false;
    if (header.fragmentCount == 0 || header.fragmentCount > kMaxVideoFragments || header.fragmentIndex >= header.fragmentCount) return false;
    if (header.type == MediaType::Audio) {
        const auto allowed = static_cast<std::uint8_t>(kMediaFlagSystemAudio);
        if ((header.flags & static_cast<std::uint8_t>(~allowed)) != 0) return false;
        return header.fragmentCount == 1 && header.fragmentIndex == 0 && header.frameId == 0;
    }
    if (header.type == MediaType::Keepalive) {
        return header.flags == 0 && header.payloadSize == 0 && header.fragmentCount == 1 &&
               header.fragmentIndex == 0 && header.frameId == 0;
    }
    if ((header.flags & static_cast<std::uint8_t>(~kMediaFlagKeyframe)) != 0) return false;
    return header.frameId != 0;
}
} // namespace

void Writer::U8(const std::uint8_t value) { data_.push_back(value); }
void Writer::U16(const std::uint16_t value) { U8(static_cast<std::uint8_t>(value >> 8)); U8(static_cast<std::uint8_t>(value)); }
void Writer::U32(const std::uint32_t value) { for (int shift = 24; shift >= 0; shift -= 8) U8(static_cast<std::uint8_t>(value >> shift)); }
void Writer::U64(const std::uint64_t value) { for (int shift = 56; shift >= 0; shift -= 8) U8(static_cast<std::uint8_t>(value >> shift)); }
bool Writer::String(const std::string& value, const std::size_t maxLength) {
    if (value.size() > maxLength || value.size() > UINT16_MAX) return false;
    U16(static_cast<std::uint16_t>(value.size()));
    data_.insert(data_.end(), value.begin(), value.end());
    return true;
}
void Writer::Bytes(const std::span<const std::uint8_t> bytes) { data_.insert(data_.end(), bytes.begin(), bytes.end()); }
const std::vector<std::uint8_t>& Writer::Data() const noexcept { return data_; }

Reader::Reader(const std::span<const std::uint8_t> bytes) noexcept : bytes_(bytes) {}
bool Reader::U8(std::uint8_t& value) { if (Remaining() < 1) return false; value = bytes_[offset_++]; return true; }
bool Reader::U16(std::uint16_t& value) { std::uint8_t a, b; if (!U8(a) || !U8(b)) return false; value = static_cast<std::uint16_t>((a << 8) | b); return true; }
bool Reader::U32(std::uint32_t& value) { if (Remaining() < 4) return false; value = 0; for (int i = 0; i < 4; ++i) value = (value << 8) | bytes_[offset_++]; return true; }
bool Reader::U64(std::uint64_t& value) { if (Remaining() < 8) return false; value = 0; for (int i = 0; i < 8; ++i) value = (value << 8) | bytes_[offset_++]; return true; }
bool Reader::String(std::string& value, const std::size_t maxLength) {
    std::uint16_t size;
    if (!U16(size) || size > maxLength || Remaining() < size) return false;
    value.assign(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
    offset_ += size;
    return true;
}
std::size_t Reader::Remaining() const noexcept { return bytes_.size() - offset_; }

std::vector<std::uint8_t> EncodeControl(const ControlMessage& message) {
    if (message.payload.size() > kMaxControlPayload || !IsControlType(static_cast<std::uint8_t>(message.type))) return {};
    Writer body;
    body.U32(kControlMagic);
    body.U16(kProtocolVersion);
    body.U8(static_cast<std::uint8_t>(message.type));
    body.U32(message.requestId);
    body.U32(static_cast<std::uint32_t>(message.payload.size()));
    body.Bytes(message.payload);
    Writer framed;
    framed.U32(static_cast<std::uint32_t>(body.Data().size()));
    framed.Bytes(body.Data());
    return framed.Data();
}

std::optional<ControlMessage> DecodeControl(const std::span<const std::uint8_t> packet) {
    if (packet.size() < kControlHeaderSize) return std::nullopt;
    Reader reader(packet);
    std::uint32_t magic, requestId, payloadSize;
    std::uint16_t version;
    std::uint8_t rawType;
    if (!reader.U32(magic) || !reader.U16(version) || !reader.U8(rawType) || !reader.U32(requestId) || !reader.U32(payloadSize)) return std::nullopt;
    if (magic != kControlMagic || version != kProtocolVersion || !IsControlType(rawType) || payloadSize > kMaxControlPayload || reader.Remaining() != payloadSize) return std::nullopt;
    ControlMessage message{static_cast<ControlType>(rawType), requestId, {}};
    message.payload.assign(packet.end() - static_cast<std::ptrdiff_t>(payloadSize), packet.end());
    return message;
}

std::optional<ControlMessage> TryReadControl(std::vector<std::uint8_t>& stream) {
    if (stream.size() < 4) return std::nullopt;
    Reader prefix(std::span<const std::uint8_t>(stream.data(), 4));
    std::uint32_t bodySize;
    if (!prefix.U32(bodySize) || bodySize < kControlHeaderSize || bodySize > kControlHeaderSize + kMaxControlPayload) {
        stream.clear();
        return std::nullopt;
    }
    if (stream.size() < bodySize + 4) return std::nullopt;
    const auto message = DecodeControl(std::span<const std::uint8_t>(stream.data() + 4, bodySize));
    stream.erase(stream.begin(), stream.begin() + static_cast<std::ptrdiff_t>(bodySize + 4));
    return message;
}

std::vector<std::uint8_t> EncodeMediaHeader(const MediaHeader& header) {
    if (!IsValidMediaHeader(header)) return {};
    Writer writer;
    writer.U32(kMediaMagic);
    writer.U16(kProtocolVersion);
    writer.U8(static_cast<std::uint8_t>(header.type));
    writer.U8(header.flags);
    writer.U32(header.sessionId);
    writer.U32(header.sequence);
    writer.U64(header.timestamp);
    writer.U32(header.frameId);
    writer.U16(header.fragmentIndex);
    writer.U16(header.fragmentCount);
    writer.U16(header.payloadSize);
    return writer.Data();
}

std::optional<MediaHeader> DecodeMediaHeader(const std::span<const std::uint8_t> packet) {
    if (packet.size() < kMediaHeaderSize) return std::nullopt;
    Reader reader(packet);
    std::uint32_t magic, sessionId, sequence, frameId;
    std::uint16_t version, fragmentIndex, fragmentCount, payloadSize;
    std::uint8_t rawType, flags;
    std::uint64_t timestamp;
    if (!reader.U32(magic) || !reader.U16(version) || !reader.U8(rawType) || !reader.U8(flags) ||
        !reader.U32(sessionId) || !reader.U32(sequence) || !reader.U64(timestamp) || !reader.U32(frameId) ||
        !reader.U16(fragmentIndex) || !reader.U16(fragmentCount) || !reader.U16(payloadSize)) return std::nullopt;
    if (magic != kMediaMagic || version != kProtocolVersion || !IsMediaType(rawType) || reader.Remaining() != payloadSize) return std::nullopt;
    MediaHeader header{static_cast<MediaType>(rawType), sessionId, sequence, timestamp, payloadSize, flags, frameId, fragmentIndex, fragmentCount};
    if (!IsValidMediaHeader(header)) return std::nullopt;
    return header;
}

} // namespace vibez::protocol
