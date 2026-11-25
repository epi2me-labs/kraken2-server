#include "server/stream_session.h"

#include <sys/time.h>

#include <iostream>
#include <thread>

#include "core/classifier.h"

using kraken2proto::Kraken2SequenceRequest;
using kraken2proto::Kraken2SequenceRequestMulti;
using kraken2proto::Kraken2SequenceStreamResult;

StreamSession::StreamSession(Engine &engine, grpc::ServerContext *context, ClassifyStreamIO *stream)
    : engine_(engine), context_(context), stream_(stream)
{
}

void StreamSession::Fail(const std::string &message)
{
    std::lock_guard<std::mutex> lock(failure_mutex_);
    if (failure_.empty())
        failure_ = message.empty() ? "unknown error" : message;
}

bool StreamSession::failed() const
{
    std::lock_guard<std::mutex> lock(failure_mutex_);
    return !failure_.empty();
}

grpc::Status StreamSession::Run(std::string &report)
{
    std::cerr << "Starting stream handler." << std::endl;
    stream_->SendInitialMetadata();

    struct timeval start, end;
    gettimeofday(&start, nullptr);

    std::thread writer([this] { WriteResults(); });
    try {
        // Classify while reads are still being received. Submitting waits when
        // the pool is busy, which holds back reading and so, through gRPC's flow
        // control, the client.
        Kraken2SequenceRequestMulti batch;
        while (!context_->IsCancelled() && !failed() && stream_->Read(&batch)) {
            {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                ++pending_;
            }
            bool queued = engine_.pool().Submit(
                [this, reads = std::move(batch)]() {
                    ClassifyBatch(reads);
                    {
                        std::lock_guard<std::mutex> lock(pending_mutex_);
                        --pending_;
                    }
                    pending_done_.notify_all();
                });
            if (!queued) {   // the server is shutting down
                Fail("server is shutting down");
                std::lock_guard<std::mutex> lock(pending_mutex_);
                --pending_;
            }
        }
    }
    catch (...) {
        // the writer must be joined before it is destroyed
        Fail("stream handling failed");
    }

    // wait for the batches in flight, then for the writer to send what they produced
    {
        std::unique_lock<std::mutex> lock(pending_mutex_);
        pending_done_.wait(lock, [this] { return pending_ == 0; });
    }
    results_.close();
    writer.join();

    gettimeofday(&end, nullptr);
    if (failed()) {
        std::lock_guard<std::mutex> lock(failure_mutex_);
        std::cerr << "Stream failed: " << failure_ << std::endl;
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, failure_);
    }

    const double seconds = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1e6;
    report = k2s::FormatReport(engine_.index(), tally_, engine_.config().report);
    std::cerr << k2s::DescribeRun(tally_.stats, seconds) << std::endl;
    if (engine_.config().keep_history)
        engine_.history().Add(std::move(tally_));
    std::cerr << "Finished stream handler." << std::endl;
    return grpc::Status::OK;
}

void StreamSession::ClassifyBatch(const Kraken2SequenceRequestMulti &batch)
{
    try {
        if (failed())
            return;

        // a classifier is cheap, and not shareable, so each batch has its own
        k2s::Classifier classifier(engine_.index(), engine_.config().classify);
        k2s::Classification found;
        std::string hitlist;

        BatchResult result;
        result.classifications.mutable_classes()->Reserve(batch.seqs_size());
        for (const Kraken2SequenceRequest &request : batch.seqs()) {
            k2s::Read read{request.id(), request.seq(), request.quals(),
                           request.format() == Kraken2SequenceRequest::FORMAT_FASTQ};
            result.tally.stats.sequences++;
            result.tally.stats.bases += request.seq().size();

            bool classified = classifier.Classify(read, found, result.tally.counters);
            result.tally.stats.classified += classified;

            auto *out = result.classifications.add_classes();
            out->set_id(request.id());
            out->set_classified(classified);
            if (classified) {
                out->set_tax_id(classifier.ExternalId(found.taxon));
                out->set_name(classifier.Name(found.taxon));
            }
            out->set_size(request.seq().size());
            hitlist.clear();
            classifier.AppendHitlist(found, hitlist);
            out->set_hitlist(hitlist);
        }
        results_.push(std::move(result));
    }
    catch (const std::exception &ex) {
        Fail(ex.what());
    }
    catch (...) {
        Fail("unknown error while classifying");
    }
}

void StreamSession::WriteResults()
{
    bool client_writable = true;
    while (auto result = results_.pop()) {
        if (client_writable) {
            // The client has said it can receive messages as large as a batch
            Kraken2SequenceStreamResult message;
            message.mutable_classifications()->Swap(&result->classifications);
            // Keep draining if the client has gone, so the totals stay right.
            client_writable = stream_->Write(message, grpc::WriteOptions().set_buffer_hint());
        }
        tally_.Merge(std::move(result->tally));
    }
}
