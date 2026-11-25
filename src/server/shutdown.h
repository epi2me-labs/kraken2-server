#pragma once

#include <pthread.h>

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <iostream>
#include <mutex>
#include <thread>

/**
 * A one-way latch used to ask the server to stop. Requesting is idempotent and
 * may come from several places (signals, a remote shutdown request).
 */
class ShutdownLatch
{
public:
    void Request()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            requested_ = true;
        }
        cv_.notify_all();
    }

    void Wait()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return requested_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool requested_ = false;
};

/**
 * Turns SIGINT/SIGTERM/SIGQUIT into a request to shut down.
 *
 * The signals are blocked in the calling thread (and so in every thread created
 * after it), and a dedicated thread waits for them synchronously. That avoids
 * doing anything but the most trivial work in an asynchronous signal handler.
 * Construct this before creating any other threads.
 */
class SignalWatcher
{
public:
    explicit SignalWatcher(ShutdownLatch &shutdown)
    {
        sigemptyset(&set_);
        sigaddset(&set_, SIGINT);
        sigaddset(&set_, SIGTERM);
        sigaddset(&set_, SIGQUIT);
        // A shell starts background jobs with SIGINT ignored, and ignored
        // signals are never delivered to sigwait.
        for (int sig : {SIGINT, SIGTERM, SIGQUIT})
            signal(sig, SIG_DFL);
        pthread_sigmask(SIG_BLOCK, &set_, nullptr);
        thread_ = std::thread([this, &shutdown]() {
            int sig;
            if (sigwait(&set_, &sig) == 0) {
                if (!stopping_)
                    std::cerr << "Received signal " << sig << "." << std::endl;
                shutdown.Request();
            }
            done_ = true;
        });
    }

    ~SignalWatcher()
    {
        // wake the watcher thread if no signal ever arrived
        stopping_ = true;
        if (!done_)
            pthread_kill(thread_.native_handle(), SIGTERM);
        thread_.join();
    }

    SignalWatcher(const SignalWatcher &) = delete;
    SignalWatcher &operator=(const SignalWatcher &) = delete;

private:
    sigset_t set_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> done_{false};
    std::thread thread_;
};
