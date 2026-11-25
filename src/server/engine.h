#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include "core/index.h"
#include "core/tally.h"
#include "common/worker_pool.h"
#include "server/config.h"

enum class IndexState { Loading, Ready, Failed };

/**
 * What the server shares between all its clients: the database, the threads
 * that classify, and the running totals.
 *
 * The database is loaded on a background thread, so the server can answer
 * "not ready yet" straight away rather than refuse connections for as long as
 * a large database takes to read. Check state() before using index().
 */
class Engine
{
public:
    explicit Engine(const ServerConfig &config);
    ~Engine();

    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    const ServerConfig &config() const { return config_; }
    IndexState state() const { return state_.load(); }

    /// Only valid once state() is Ready.
    const k2s::Index &index() const { return *index_; }

    k2s::WorkerPool &pool() { return pool_; }
    k2s::History &history() { return history_; }

private:
    void Load();

    const ServerConfig config_;
    std::atomic<IndexState> state_{IndexState::Loading};
    // set by the loading thread before state_ becomes Ready
    std::unique_ptr<k2s::Index> index_;
    k2s::WorkerPool pool_;
    k2s::History history_;
    std::thread loader_;   // last, so everything else exists before it starts
};
