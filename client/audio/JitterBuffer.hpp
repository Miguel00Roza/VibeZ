#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace vibez::audio {

class JitterBuffer {
public:
    explicit JitterBuffer(const std::size_t capacity) : capacity_(capacity) {}

    [[nodiscard]] bool Push(const std::uint32_t sequence, const std::span<const std::uint8_t> packet) {
        std::lock_guard lock(mutex_);
        if (packet.empty() || capacity_ == 0) return false;
        if (!nextSequence_) nextSequence_ = sequence;
        const auto distance = static_cast<std::int32_t>(sequence - *nextSequence_);
        if (distance < 0 || static_cast<std::size_t>(distance) >= capacity_) return false;
        return packets_.emplace(sequence, std::vector<std::uint8_t>(packet.begin(), packet.end())).second;
    }

    [[nodiscard]] std::optional<std::vector<std::uint8_t>> PopNext() {
        std::lock_guard lock(mutex_);
        if (!nextSequence_ || packets_.empty()) return std::nullopt;
        auto packet = packets_.find(*nextSequence_);
        std::optional<std::vector<std::uint8_t>> result;
        if (packet != packets_.end()) {
            result = std::move(packet->second);
            packets_.erase(packet);
        } else {
            auto nextAvailable = packets_.end();
            auto smallestDistance = INT32_MAX;
            for (auto candidate = packets_.begin(); candidate != packets_.end(); ++candidate) {
                const auto distance = static_cast<std::int32_t>(candidate->first - *nextSequence_);
                if (distance > 0 && distance < smallestDistance) {
                    nextAvailable = candidate;
                    smallestDistance = distance;
                }
            }
            if (nextAvailable == packets_.end()) return std::nullopt;
            result = std::vector<std::uint8_t>{};
        }
        ++*nextSequence_;
        return result;
    }

    [[nodiscard]] std::size_t Size() const {
        std::lock_guard lock(mutex_);
        return packets_.size();
    }
    [[nodiscard]] bool Primed() const {
        std::lock_guard lock(mutex_);
        return nextSequence_.has_value();
    }
    [[nodiscard]] std::uint32_t NextSequence() const {
        std::lock_guard lock(mutex_);
        return nextSequence_.value_or(0);
    }
    void Reset() {
        std::lock_guard lock(mutex_);
        packets_.clear();
        nextSequence_.reset();
    }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::map<std::uint32_t, std::vector<std::uint8_t>> packets_;
    std::optional<std::uint32_t> nextSequence_;
};

} // namespace vibez::audio