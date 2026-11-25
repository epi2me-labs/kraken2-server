#include <atomic>
#include <chrono>
#include <future>
#include <thread>

#include "check.h"
#include "common/worker_pool.h"

using k2s::WorkerPool;
using namespace std::chrono_literals;

int main()
{
    {   // every task runs, on the requested number of threads
        std::atomic<int> ran{0};
        {
            WorkerPool pool(4, 16);
            CHECK_EQ(pool.threads(), 4u);
            for (int i = 0; i < 1000; ++i)
                CHECK(pool.Submit([&] { ++ran; }));
        }   // the destructor finishes what was queued
        CHECK_EQ(ran.load(), 1000);
    }
    {   // submitting waits when every worker is busy and the queue is full
        std::promise<void> release;
        auto released = release.get_future().share();
        std::atomic<int> ran{0};
        WorkerPool pool(1, 1);
        pool.Submit([&] { released.wait(); ++ran; });   // occupies the worker
        std::this_thread::sleep_for(50ms);
        pool.Submit([&] { ++ran; });                    // fills the queue
        std::atomic<bool> third_submitted{false};
        std::thread submitter([&] { pool.Submit([&] { ++ran; }); third_submitted = true; });
        std::this_thread::sleep_for(100ms);
        CHECK(!third_submitted);   // held up: this is the backpressure
        release.set_value();
        submitter.join();
        CHECK(third_submitted);
    }
    {   // a task that throws is dropped; the pool carries on
        std::atomic<int> ran{0};
        {
            WorkerPool pool(2, 4);
            pool.Submit([] { throw std::runtime_error("boom"); });
            pool.Submit([] { throw 42; });
            for (int i = 0; i < 10; ++i)
                pool.Submit([&] { ++ran; });
        }
        CHECK_EQ(ran.load(), 10);
    }
    {   // 0 threads means one per hardware thread
        WorkerPool pool(0, 0);
        CHECK(pool.threads() >= 1);
    }
    return finish("worker_pool_test");
}
