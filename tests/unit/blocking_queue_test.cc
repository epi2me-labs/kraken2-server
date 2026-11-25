#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "check.h"
#include "common/blocking_queue.h"

using k2s::BlockingQueue;
using namespace std::chrono_literals;

int main()
{
    {   // items come out in the order they went in
        BlockingQueue<int> q;
        for (int i = 0; i < 5; ++i)
            CHECK(q.push(i));
        CHECK_EQ(q.size(), 5u);
        for (int i = 0; i < 5; ++i)
            CHECK_EQ(*q.pop(), i);
    }
    {   // closing lets consumers drain what is left, then ends them
        BlockingQueue<int> q;
        q.push(1);
        q.push(2);
        q.close();
        CHECK(!q.push(3));  // and refuses anything more
        CHECK_EQ(*q.pop(), 1);
        CHECK_EQ(*q.pop(), 2);
        CHECK(!q.pop().has_value());
    }
    {   // closing wakes a consumer that is waiting
        BlockingQueue<int> q;
        std::atomic<bool> got_end{false};
        std::thread consumer([&] { got_end = !q.pop().has_value(); });
        std::this_thread::sleep_for(50ms);
        CHECK(!got_end);
        q.close();
        consumer.join();
        CHECK(got_end);
    }
    {   // a bounded queue makes producers wait until there is room
        BlockingQueue<int> q(2);
        q.push(1);
        q.push(2);
        std::atomic<bool> pushed{false};
        std::thread producer([&] { q.push(3); pushed = true; });
        std::this_thread::sleep_for(50ms);
        CHECK(!pushed);   // still blocked: the queue is full
        CHECK_EQ(*q.pop(), 1);
        producer.join();
        CHECK(pushed);
        CHECK_EQ(*q.pop(), 2);
        CHECK_EQ(*q.pop(), 3);
    }
    {   // closing wakes a producer that is waiting for room, and its item is dropped
        BlockingQueue<int> q(1);
        q.push(1);
        std::atomic<int> result{-1};
        std::thread producer([&] { result = q.push(2) ? 1 : 0; });
        std::this_thread::sleep_for(50ms);
        q.close();
        producer.join();
        CHECK_EQ(result.load(), 0);
    }
    {   // many producers and consumers: everything arrives exactly once
        BlockingQueue<int> q(8);
        const int producers = 4, per_producer = 2000;
        std::atomic<long> sum{0};
        std::atomic<int> count{0};
        std::vector<std::thread> threads;
        for (int c = 0; c < 3; ++c)
            threads.emplace_back([&] {
                while (auto item = q.pop()) { sum += *item; ++count; }
            });
        std::vector<std::thread> prods;
        for (int p = 0; p < producers; ++p)
            prods.emplace_back([&] { for (int i = 1; i <= per_producer; ++i) q.push(i); });
        for (auto &t : prods) t.join();
        q.close();
        for (auto &t : threads) t.join();
        CHECK_EQ(count.load(), producers * per_producer);
        CHECK_EQ(sum.load(), static_cast<long>(producers) * per_producer * (per_producer + 1) / 2);
    }
    {   // move-only items work
        BlockingQueue<std::unique_ptr<int>> q;
        q.push(std::make_unique<int>(7));
        CHECK_EQ(**q.pop(), 7);
    }
    return finish("blocking_queue_test");
}
