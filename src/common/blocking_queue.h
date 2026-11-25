#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <queue>
#include <utility>

namespace k2s {

/**
 * A multi-producer, multi-consumer queue with blocking operations.
 *
 * pop() blocks until an item is available, and returns std::nullopt only once
 * the queue has been closed *and* drained. push() blocks while a bounded queue
 * is full and returns false if the queue has been closed. Closing wakes
 * everyone, so either side can use it to signal "no more data" or "stop".
 */
template <typename T>
class BlockingQueue
{
public:
    /// @param capacity maximum number of queued items; 0 means unbounded.
    explicit BlockingQueue(std::size_t capacity = 0) : capacity_(capacity) {}

    BlockingQueue(const BlockingQueue &) = delete;
    BlockingQueue &operator=(const BlockingQueue &) = delete;

    /// @return false if the queue was closed (the item is dropped).
    bool push(T item)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        not_full_.wait(lock, [this] {
            return closed_ || capacity_ == 0 || queue_.size() < capacity_;
        });
        if (closed_)
            return false;
        queue_.push(std::move(item));
        not_empty_.notify_one();
        return true;
    }

    /// Blocks for an item. std::nullopt means closed and empty.
    std::optional<T> pop()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [this] { return closed_ || !queue_.empty(); });
        if (queue_.empty())
            return std::nullopt;
        T item = std::move(queue_.front());
        queue_.pop();
        not_full_.notify_one();
        return item;
    }

    /// No further pushes are accepted; consumers drain what is left.
    void close()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    std::size_t size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::queue<T> queue_;
    bool closed_ = false;
};

}  // namespace k2s
