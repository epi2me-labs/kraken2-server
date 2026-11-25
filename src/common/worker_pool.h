#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iostream>
#include <thread>
#include <vector>

#include "common/blocking_queue.h"

namespace k2s {

/**
 * A fixed set of threads running tasks from a bounded queue.
 *
 * Submit() blocks while the queue is full. That is deliberate: whoever feeds the
 * pool (for the server, the thread reading a client's stream) is slowed to the
 * pool's pace, so a fast client cannot make the server queue up unbounded work.
 *
 * Tasks must not throw; one that does is reported and dropped rather than
 * taking the process down.
 */
class WorkerPool
{
public:
    /// @param threads number of worker threads; 0 means one per hardware thread.
    /// @param queue_capacity tasks that may wait for a free worker; 0 means unbounded.
    WorkerPool(std::size_t threads, std::size_t queue_capacity)
        : queue_(queue_capacity)
    {
        if (threads == 0)
            threads = std::max(1u, std::thread::hardware_concurrency());
        for (std::size_t i = 0; i < threads; ++i)
            workers_.emplace_back([this] { Work(); });
    }

    /// Runs every task already queued, then stops the workers.
    ~WorkerPool()
    {
        queue_.close();
        for (auto &t : workers_)
            t.join();
    }

    WorkerPool(const WorkerPool &) = delete;
    WorkerPool &operator=(const WorkerPool &) = delete;

    /// Queue a task, waiting for room. Returns false if the pool is shutting down.
    bool Submit(std::function<void()> task) { return queue_.push(std::move(task)); }

    std::size_t threads() const { return workers_.size(); }

private:
    void Work()
    {
        while (auto task = queue_.pop()) {
            try {
                (*task)();
            }
            catch (const std::exception &ex) {
                std::cerr << "Worker task failed: " << ex.what() << std::endl;
            }
            catch (...) {
                std::cerr << "Worker task failed with an unknown error." << std::endl;
            }
        }
    }

    BlockingQueue<std::function<void()>> queue_;
    std::vector<std::thread> workers_;
};

}  // namespace k2s
