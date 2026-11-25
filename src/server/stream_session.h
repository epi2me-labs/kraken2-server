#pragma once

#include <grpcpp/grpcpp.h>

#include <condition_variable>
#include <mutex>
#include <string>

#include "Kraken2.grpc.pb.h"
#include "common/blocking_queue.h"
#include "core/tally.h"
#include "server/engine.h"

using ClassifyStreamIO = grpc::ServerReaderWriter<kraken2proto::Kraken2SequenceStreamResult,
                                                  kraken2proto::Kraken2SequenceRequestMulti>;

/**
 * One client's classification stream, from its first batch of reads to the report.
 *
 * Batches of reads are classified on the engine's shared worker pool, and a
 * thread of the session's own writes the results back as they are produced.
 * Everything that can go wrong with the stream is handled here, and comes out
 * as the status that Run() returns.
 */
class StreamSession
{
public:
    StreamSession(Engine &engine, grpc::ServerContext *context, ClassifyStreamIO *stream);

    /**
     * Classify everything the client sends, until it has sent it all.
     *
     * @param report receives the Kraken-style report for the stream, on success
     * @return OK (also if the client went away part way: what was classified still
     *         counts towards the server's totals), or INVALID_ARGUMENT if a read
     *         could not be classified, which stops the stream at that point
     */
    grpc::Status Run(std::string &report);

private:
    struct BatchResult
    {
        kraken2proto::Kraken2SequenceResultMulti classifications;
        k2s::Tally tally;
    };

    void ClassifyBatch(const kraken2proto::Kraken2SequenceRequestMulti &batch);
    void WriteResults();
    void Fail(const std::string &message);
    bool failed() const;

    Engine &engine_;
    grpc::ServerContext *context_;
    ClassifyStreamIO *stream_;

    k2s::BlockingQueue<BatchResult> results_;
    k2s::Tally tally_;   // for the whole stream; only the writer thread touches it

    // the first thing that went wrong, if anything
    mutable std::mutex failure_mutex_;
    std::string failure_;

    // batches handed to the pool and not finished yet
    std::mutex pending_mutex_;
    std::condition_variable pending_done_;
    size_t pending_ = 0;
};
