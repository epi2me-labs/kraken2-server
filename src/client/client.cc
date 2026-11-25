#include "client.h"

#include <sysexits.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <optional>
#include <thread>

#include "sequence_reader.h"

using namespace std::chrono_literals;
using grpc::ClientContext;
using grpc::Status;
using namespace kraken2proto;

namespace {

const size_t kSendBatchSize = 2000;      // reads in a gRPC message
const uint64_t kMaxInFlight = 64000;     // reads sent and not yet answered
const size_t kReadBatchSize = 4000;      // reads taken from the file at once
const size_t kMaxQueuedBatches = 64;     // batches read ahead of the sender
// Messages must stay below the size the server accepts (see server/main.cc)
const uint64_t kMaxMessageBytes = 128 * 1024 * 1024;

void PrintClassification(const Kraken2SequenceResult &c)
{
    std::cout << (c.classified() ? 'C' : 'U') << '\t' << c.id() << '\t' << c.tax_id() << '\t'
              << c.size() << '\t' << c.hitlist() << '\n';
}

void WriteReport(const std::string &report, const std::string &report_file)
{
    if (report_file.empty())
        return;
    std::ofstream out(report_file);
    out << report;
    if (!out)
        std::cerr << "Failed to write report file: " << report_file << std::endl;
}

}  // namespace

Client::Client(std::shared_ptr<grpc::Channel> channel) : stub_(Kraken2Service::NewStub(channel)) {}

int Client::WaitForServer()
{
    while (true) {
        ClientContext context;
        Kraken2ReadyRequest request;
        Kraken2ReadyResult response;
        Status status = stub_->ServerReady(&context, request, &response);
        if (status.ok()) {
            std::cerr << "Server responded as ready." << std::endl;
            return EX_OK;
        }
        if (status.error_code() != grpc::StatusCode::UNAVAILABLE) {
            std::cerr << "Server is in error state: " << status.error_message() << std::endl;
            return status.error_code();
        }
        // not ready yet, or not up yet: it may come back
        std::cerr << "Server is not ready: " << status.error_message() << std::endl;
        std::cerr << "Waiting 2s..." << std::endl;
        std::this_thread::sleep_for(2s);
    }
}

int Client::Classify(const std::string &sequence_file, const std::string &report_file)
{
    std::cerr << "Classifying sequence stream." << std::endl;
    int state = WaitForServer();
    if (state != EX_OK)
        return state;

    ClientContext context;
    Stream stream(stub_->ClassifyStream(&context));
    InFlightLimiter limiter(kMaxInFlight);
    k2s::BlockingQueue<SequenceBatch> batches(kMaxQueuedBatches);
    std::atomic<bool> read_failed{false};

    auto reading = std::async(std::launch::async, [&] { return ReadBatches(sequence_file, batches, read_failed); });
    auto sending = std::async(std::launch::async, [&] { return SendBatches(batches, limiter, stream); });
    auto receiving = std::async(std::launch::async, [&] { return ReceiveResults(limiter, report_file, stream); });

    reading.wait();
    uint64_t sent = sending.get();
    uint64_t received = receiving.get();
    std::cout.flush();
    std::cerr << "Sent    : " << sent << std::endl;
    std::cerr << "Received: " << received << std::endl;
    if (received != sent)
        std::cerr << "Warning: not all sequences sent were classified." << std::endl;

    Status status = stream->Finish();
    if (!status.ok()) {
        std::cerr << "Client RPC stream failed: " << status.error_message() << std::endl;
        return status.error_code();
    }
    return read_failed ? EX_IOERR : EX_OK;
}

uint64_t Client::ReadBatches(const std::string &sequence_file,
                             k2s::BlockingQueue<SequenceBatch> &batches,
                             std::atomic<bool> &read_failed)
{
    uint64_t total = 0;
    try {
        SequenceReader reader(sequence_file);
        std::cerr << "Reading sequences from file: " << sequence_file << std::endl;
        while (true) {
            SequenceBatch batch;
            if (reader.ReadBatch(batch, kReadBatchSize) == 0)
                break;
            total += batch.size();
            // blocks while the queue is full, and fails if the sender has stopped
            if (!batches.push(std::move(batch)))
                break;
        }
    }
    catch (const std::exception &ex) {
        std::cerr << "Failed to read sequences from file: " << sequence_file << ": " << ex.what()
                  << std::endl;
        read_failed = true;
    }
    batches.close();
    return total;
}

uint64_t Client::SendBatches(k2s::BlockingQueue<SequenceBatch> &batches, InFlightLimiter &limiter,
                             Stream &stream)
{
    uint64_t sent = 0;
    bool ok = true;
    while (ok) {
        std::optional<SequenceBatch> item = batches.pop();
        if (!item.has_value())
            break;   // everything has been read, and sent
        SequenceBatch &batch = *item;
        if (!limiter.Acquire(batch.size())) {
            ok = false;
            break;
        }
        // gRPC messages are smaller than the batches read from the file
        for (size_t i = 0; i < batch.size() && ok; i += kSendBatchSize) {
            const size_t last = std::min(batch.size(), i + kSendBatchSize);
            Kraken2SequenceRequestMulti message;
            for (size_t k = i; k < last; ++k)
                message.add_seqs()->Swap(&batch[k]);
            if (message.ByteSizeLong() <= kMaxMessageBytes) {
                ok = stream->Write(message);
                sent += last - i;
                continue;
            }
            // too large for one message, so one read at a time
            for (auto &seq : *message.mutable_seqs()) {
                Kraken2SequenceRequestMulti single;
                single.add_seqs()->Swap(&seq);
                if (single.ByteSizeLong() > kMaxMessageBytes) {
                    std::cerr << "Read is too large! Skipping." << std::endl;
                    limiter.Release(1);
                    continue;
                }
                ok = stream->Write(single, grpc::WriteOptions().set_buffer_hint());
                if (!ok)
                    break;
                sent++;
            }
        }
    }
    if (!ok) {
        std::cerr << "Failed to send sequences to the server." << std::endl;
        batches.close();   // unblock the reader
    }
    stream->WritesDone();
    return sent;
}

uint64_t Client::ReceiveResults(InFlightLimiter &limiter, const std::string &report_file,
                                Stream &stream)
{
    Kraken2SequenceStreamResult result;
    uint64_t received = 0;
    while (stream->Read(&result)) {
        if (result.has_classifications()) {
            for (const auto &classification : result.classifications().classes()) {
                received++;
                PrintClassification(classification);
            }
            limiter.Release(result.classifications().classes_size());
        }
        else if (result.has_summary()) {
            WriteReport(result.summary(), report_file);
        }
        else {
            std::cerr << "Result had neither classifications or summary :/" << std::endl;
        }
    }
    // If the stream ended early, don't leave the sender waiting for replies.
    limiter.Abort();
    return received;
}

int Client::PrintSummary()
{
    ClientContext context;
    Kraken2SummaryRequest request;
    Kraken2SummaryResults response;
    Status status = stub_->GetSummary(&context, request, &response);
    if (!status.ok())
        std::cerr << "Could not retrieve Kraken2 server summary." << std::endl;
    std::cout << response.summary() << std::endl;
    return status.error_code();
}

int Client::Shutdown()
{
    ClientContext context;
    Kraken2ShutdownRequest request;
    Kraken2ShutdownResult response;
    Status status = stub_->RemoteShutdown(&context, request, &response);
    if (!status.ok())
        std::cerr << "Failed to send shutdown request." << std::endl;
    if (response.successful())
        std::cerr << "Shutdown request processed." << std::endl;
    else
        std::cerr << "Shutdown request not processed correctly." << std::endl;
    return status.error_code();
}
