#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <queue>
#include <utility>

namespace vibez::audio {

template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(const std::size_t capacity) : capacity_(capacity) {}

    [[nodiscard]] bool TryPush(T value) {
        std::lock_guard lock(mutex_);
        if (capacity_ == 0 || queue_.size() >= capacity_) return false;
        queue_.push(std::move(value));
        return true;
    }

    [[nodiscard]] std::optional<T> TryPop() {
        std::lock_guard lock(mutex_);
        if (queue_.empty()) return std::nullopt;
        T value = std::move(queue_.front());
        queue_.pop();
        return value;
    }

    [[nodiscard]] std::size_t Size() const {
        std::lock_guard lock(mutex_);
        return queue_.size();
    }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::queue<T> queue_;
};

// Blocking variant for worker threads. Close() wakes every waiter and makes
// future pushes fail, so Stop() never depends on a producer sending one more
// item.
template <typename T>
class BlockingQueue {
public:
    explicit BlockingQueue(const std::size_t capacity) : capacity_(capacity) {}

    [[nodiscard]] bool TryPush(T value) {
        std::lock_guard lock(mutex_);
        if (closed_ || capacity_ == 0 || queue_.size() >= capacity_) return false;
        queue_.push(std::move(value));
        ready_.notify_one();
        return true;
    }

    [[nodiscard]] bool WaitPop(T& value, const std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        if (!ready_.wait_for(lock, timeout, [this] { return closed_ || !queue_.empty(); })) return false;
        if (queue_.empty()) return false;
        value = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    void Close() {
        std::lock_guard lock(mutex_);
        closed_ = true;
        ready_.notify_all();
    }

    void Reopen() {
        std::lock_guard lock(mutex_);
        closed_ = false;
    }

    void Clear() {
        std::lock_guard lock(mutex_);
        std::queue<T> empty;
        queue_.swap(empty);
    }

    [[nodiscard]] bool Closed() const {
        std::lock_guard lock(mutex_);
        return closed_;
    }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::queue<T> queue_;
    bool closed_{};
};

} // namespace vibez::audio