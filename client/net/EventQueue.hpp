#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace vibez::client {

// Thread-safe event queue shared between the network/media threads and the UI
// or console thread. When full, the oldest event is dropped so freshly
// received state (presence, errors) is never hidden behind stale history.
template <typename T>
class EventQueue {
public:
    explicit EventQueue(const std::size_t capacity = 512) : capacity_(capacity) {}

    void Push(T event) {
        std::lock_guard lock(mutex_);
        if (capacity_ != 0 && queue_.size() >= capacity_) queue_.pop_front();
        queue_.push_back(std::move(event));
    }

    [[nodiscard]] std::optional<T> TryPop() {
        std::lock_guard lock(mutex_);
        if (queue_.empty()) return std::nullopt;
        T event = std::move(queue_.front());
        queue_.pop_front();
        return event;
    }

    [[nodiscard]] std::vector<T> PopAll() {
        std::lock_guard lock(mutex_);
        std::vector<T> events;
        events.reserve(queue_.size());
        while (!queue_.empty()) {
            events.push_back(std::move(queue_.front()));
            queue_.pop_front();
        }
        return events;
    }

    void Clear() {
        std::lock_guard lock(mutex_);
        queue_.clear();
    }

    [[nodiscard]] std::size_t Size() const {
        std::lock_guard lock(mutex_);
        return queue_.size();
    }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<T> queue_;
};

} // namespace vibez::client
