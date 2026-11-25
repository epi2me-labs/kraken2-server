#include "server/engine.h"

#include <chrono>
#include <iostream>

namespace {

// Batches that may wait for a worker. Enough that workers are never starved,
// few enough that a flood of requests is held back rather than queued in memory.
size_t QueueCapacity(int threads)
{
    size_t workers = threads > 0 ? threads : std::max(1u, std::thread::hardware_concurrency());
    return 2 * workers;
}

}  // namespace

Engine::Engine(const ServerConfig &config)
    : config_(config), pool_(config.threads, QueueCapacity(config.threads))
{
    std::cout << "Created classification thread pool with " << pool_.threads()
              << " thread(s)." << std::endl;
    loader_ = std::thread([this] { Load(); });
}

Engine::~Engine()
{
    if (loader_.joinable())
        loader_.join();
}

void Engine::Load()
{
    std::cerr << "Loading database information..." << std::endl;
    // Delays loading, for testing clients that wait for the server.
    std::this_thread::sleep_for(std::chrono::seconds(config_.wait_seconds));

    try {
        index_.reset(new k2s::Index(config_.db_path, config_.memory_mapping));
    }
    catch (const std::exception &ex) {
        std::cerr << "Unable to load index"
                  << ": " << ex.what() << std::endl;
        state_ = IndexState::Failed;
        return;
    }
    std::cerr << "Successfully loaded index." << std::endl;
    state_ = IndexState::Ready;
}
