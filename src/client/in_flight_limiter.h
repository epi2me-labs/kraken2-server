#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>

/**
 * Bounds the number of reads that have been sent to the server but not yet
 * classified, so that a fast reader cannot flood a slow server.
 */
class InFlightLimiter
{
public:
    explicit InFlightLimiter(uint64_t limit) : limit_(limit) {}

    /// Block until n more reads fit under the limit (a batch larger than the
    /// limit is let through when nothing else is in flight). Returns false if
    /// the limiter has been aborted.
    bool Acquire(uint64_t n)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return aborted_ || in_flight_ == 0 || in_flight_ + n <= limit_; });
        if (aborted_)
            return false;
        in_flight_ += n;
        return true;
    }

    /// n of the reads have been dealt with.
    void Release(uint64_t n)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            in_flight_ = n > in_flight_ ? 0 : in_flight_ - n;
        }
        cv_.notify_all();
    }

    /// Wake and fail all current and future Acquire() calls.
    void Abort()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            aborted_ = true;
        }
        cv_.notify_all();
    }

private:
    const uint64_t limit_;
    std::mutex mutex_;
    std::condition_variable cv_;
    uint64_t in_flight_ = 0;
    bool aborted_ = false;
};
