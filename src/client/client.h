#pragma once

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "Kraken2.grpc.pb.h"
#include "common/blocking_queue.h"
#include "in_flight_limiter.h"

/// A connection to a kraken2_server.
class Client
{
public:
    explicit Client(std::shared_ptr<grpc::Channel> channel);

    /**
     * Classify the sequences in a file, printing a line for each as the answers
     * arrive, and writing the report to `report_file` if it is not empty.
     *
     * @return EX_OK on success; EX_IOERR if the sequences could not be read;
     *         EX_UNAVAILABLE if the server could not be reached; otherwise a gRPC
     *         status code
     */
    int Classify(const std::string &sequence_file, const std::string &report_file);

    /// Print the server's summary of everything it has classified.
    int PrintSummary();

    /// Ask the server to shut down.
    int Shutdown();

private:
    using SequenceBatch = std::vector<kraken2proto::Kraken2SequenceRequest>;
    using Stream = std::shared_ptr<grpc::ClientReaderWriter<
        kraken2proto::Kraken2SequenceRequestMulti, kraken2proto::Kraken2SequenceStreamResult>>;

    /// Wait until the server has loaded its database. Returns EX_OK, or an exit status.
    int WaitForServer();

    // The three stages of classifying a file, each run on its own thread.
    uint64_t ReadBatches(const std::string &sequence_file, k2s::BlockingQueue<SequenceBatch> &batches,
                         std::atomic<bool> &read_failed);
    uint64_t SendBatches(k2s::BlockingQueue<SequenceBatch> &batches, InFlightLimiter &limiter,
                         Stream &stream);
    uint64_t ReceiveResults(InFlightLimiter &limiter, const std::string &report_file,
                            Stream &stream);

    std::unique_ptr<kraken2proto::Kraken2Service::Stub> stub_;
};
