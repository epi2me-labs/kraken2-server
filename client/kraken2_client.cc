#include <atomic>
#include <cctype>
#include <cassert>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <fstream>
#include <future>
#include <getopt.h>
#include <random>
#include <thread>
#include <sysexits.h>

#include <grpc/grpc.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include "thread_safe_queue.h"
#include "Kraken2.grpc.pb.h"

#include <zlib.h>
#include "kseq.h"
#include "kseq.cc.h"

using namespace std::chrono_literals; // ns, us, ms, s, h, etc.

using grpc::Channel;
using grpc::ClientContext;
using grpc::ClientReaderWriter;
using grpc::ClientWriter;
using grpc::Status;
using grpc::WriteOptions;

using kraken2proto::Kraken2ReadyRequest;
using kraken2proto::Kraken2ReadyResult;
using kraken2proto::Kraken2SequenceRequest;
using kraken2proto::Kraken2SequenceRequestMulti;
using kraken2proto::Kraken2SequenceResult;
using kraken2proto::Kraken2SequenceResultMulti;
using kraken2proto::Kraken2SequenceStreamResult;
using kraken2proto::Kraken2Service;
using kraken2proto::Kraken2SummaryRequest;
using kraken2proto::Kraken2SummaryResults;
using kraken2proto::Kraken2ShutdownRequest;
using kraken2proto::Kraken2ShutdownResult;

// Command line options
struct Options
{
    std::string sequence;
    std::string report_file;
    std::string host = "localhost";
    int port = 8080;
    bool shutdown = false;
};

typedef std::shared_ptr<ClientReaderWriter<Kraken2SequenceRequestMulti, Kraken2SequenceStreamResult>> ClientStream;


typedef std::vector<Kraken2SequenceRequest> SequenceBatch;

/**
 * @brief Bounds the number of reads that have been sent to the server but not
 *        yet classified, so that a fast reader cannot flood a slow server.
 */
class InFlightLimiter {
public:
    explicit InFlightLimiter(uint64_t limit) : limit_(limit) {}

    /// Block until n more reads fit under the limit (a batch larger than the
    /// limit is let through when nothing else is in flight). Returns false if
    /// the limiter has been aborted.
    bool acquire(uint64_t n) {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait(lock, [&] { return aborted_ || in_flight_ == 0 || in_flight_ + n <= limit_; });
        if (aborted_) { return false; }
        in_flight_ += n;
        return true;
    }

    void release(uint64_t n) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            in_flight_ = n > in_flight_ ? 0 : in_flight_ - n;
        }
        cv_.notify_all();
    }

    /// Wake and fail all current and future acquire() calls.
    void abort() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            aborted_ = true;
        }
        cv_.notify_all();
    }

private:
    const uint64_t limit_;
    std::mutex mtx_;
    std::condition_variable cv_;
    uint64_t in_flight_ = 0;
    bool aborted_ = false;
};

#define ST_BATCH_SIZE 2000     // reads in a gRPC batch
#define MAX_IN_FLIGHT 64000    // total reads in gRPC system
#define FASTQ_BATCH_SIZE 4000  // reads to read at once from fastq
#define MAX_BATCHES 64         // number of stream batches to buffer from fastq

class SequenceClient {

public:
    SequenceClient(std::shared_ptr<Channel> channel) : sequence_stub(Kraken2Service::NewStub(channel)) {}


    /**
     * @brief Send sequences from a kseq file as a stream and receive classifications individually as a stream.
     *
     * @param sequence_name
     * @return EX_IOERR if sequences could not be read
     * @return EX_UNAVAILABLE if the server could not be reached
     * @return else gRPC status code
     */
    int ClassifySequences(const std::string &sequence_name, const std::string &report_file) {
        std::cerr << "Classifying sequence stream." << std::endl;
        int state = WaitForServer();
        if (state != 0) {return state;}

        ClientContext context;
        ClientStream stream(sequence_stub->ClassifyStream(&context));
        InFlightLimiter limiter(MAX_IN_FLIGHT);
        // batches of reads, from the file reader to the stream writer
        ThreadSafeQueue<SequenceBatch> batches_queue(MAX_BATCHES);
        std::atomic<bool> read_failed{false};

        // reads data from file into queue
        std::future<uint64_t> reading = std::async(
            std::launch::async, &SequenceClient::FastBatcher, this,
            std::cref(sequence_name), &batches_queue, std::ref(read_failed));

        // take data from queue and send over gRPC
        std::future<uint64_t> writing = std::async(
            std::launch::async, &SequenceClient::StreamWriter, this,
            &batches_queue, std::ref(limiter), std::ref(stream));

        // reading back results on gRPC stream
        std::future<uint64_t> receiving = std::async(
            std::launch::async, &SequenceClient::StreamReader, this,
            std::ref(limiter), std::cref(report_file), std::ref(stream));

        reading.wait();
        uint64_t sent = writing.get();
        uint64_t received = receiving.get();
        std::cerr << "Sent    : " << sent << std::endl;
        std::cerr << "Received: " << received << std::endl;
        if (received != sent) {
            std::cerr << "Warning: not all sequences sent were classified." << std::endl;
        }

        // Handle the stream response
        Status status = stream->Finish();
        if (!status.ok()) {
            std::cerr << "Client RPC stream failed: " << status.error_message() << std::endl;
            return status.error_code();
        }
        return read_failed ? EX_IOERR : EX_OK;
    }

    /**
     * @brief Request a summary of the classification history on the server.
     *
     * @return gRPC status code of request
     */
    int GetSummary() {
        ClientContext context;
        Kraken2SummaryRequest req;
        Kraken2SummaryResults response;

        Status status = sequence_stub->GetSummary(&context, req, &response);
        if (!status.ok())
        {
            std::cerr << "Could not retrieve Kraken2 server summary." << std::endl;
        }
        std::cout << response.summary() << std::endl;
        return status.error_code();
    }

    /**
     * @brief Shutdown the server remotely
     *
     * @return gRPC status code of request
     */
    int ShutdownServer() {
        ClientContext context;
        Kraken2ShutdownRequest req;
        Kraken2ShutdownResult response;
        Status status = sequence_stub->RemoteShutdown(&context, req, &response);
        if (!status.ok()) {
            std::cerr << "Failed to send shutdown request." << std::endl;
        }
        if (response.successful()) {
            std::cerr << "Shutdown request processed." << std::endl;
        }
        else{
            std::cerr << "Shutdown request not processed correctly." << std::endl;
        }
        return status.error_code();
    }

    /**
     * @brief Send batches from the queue on the gRPC stream, keeping the number
     *        of reads awaiting classification below a limit.
     * @return the number of sequences sent
     */
    uint64_t StreamWriter(
            ThreadSafeQueue<SequenceBatch> *batches,
            InFlightLimiter &limiter,
            ClientStream &writer) {
        // Messages must stay below the size the server accepts (see kraken2_server.cc)
        const uint64_t MAX_SIZE = 128 * 1024 * 1024;
        uint64_t seqs_sent = 0;
        bool ok = true;
        while (ok) {
            std::optional<SequenceBatch> item = batches->pop();
            if (!item.has_value()) {
                break;  // the reader has finished and everything is sent
            }
            SequenceBatch &batch = *item;
            if (!limiter.acquire(batch.size())) {
                ok = false;
                break;
            }
            // rebatch to smaller batches for stream
            for (size_t i = 0; i < batch.size() && ok; i += ST_BATCH_SIZE) {
                size_t last = std::min(batch.size(), i + ST_BATCH_SIZE);
                Kraken2SequenceRequestMulti req;
                for (size_t k = i; k < last; ++k) {
                    req.add_seqs()->Swap(&batch[k]);
                }
                if (req.ByteSizeLong() <= MAX_SIZE) {
                    ok = writer->Write(req);
                    seqs_sent += last - i;
                    continue;
                }
                // too large for one message, send one by one
                for (auto &seq : *req.mutable_seqs()) {
                    Kraken2SequenceRequestMulti single;
                    single.add_seqs()->Swap(&seq);
                    if (single.ByteSizeLong() > MAX_SIZE) {
                        std::cerr << "Read is too large! Skipping." << std::endl;
                        limiter.release(1);
                        continue;
                    }
                    ok = writer->Write(single, WriteOptions().set_buffer_hint());
                    if (!ok) { break; }
                    seqs_sent++;
                }
            }
        }
        if (!ok) {
            std::cerr << "Failed to send sequences to the server." << std::endl;
            // unblock the file reader
            batches->close();
        }
        writer->WritesDone();
        return seqs_sent;
    }

    /**
     * @brief Print classifications as they arrive, and write the final report.
     * @return the number of classifications received
     */
    uint64_t StreamReader(
            InFlightLimiter &limiter, const std::string &report_file,
            ClientStream &reader) {
        Kraken2SequenceStreamResult result;
        uint64_t n_reads = 0;
        while (reader->Read(&result)) {
            if (result.has_classifications()) {
                for (auto &res : result.classifications().classes()){
                    n_reads++;
                    PrintClassification(res);
                }
                limiter.release(result.classifications().classes_size());
            }
            else if (result.has_summary()) {
                PrintSummary(result.summary(), report_file);
            }
            else {
                std::cerr << "Result had neither classifications or summary :/" << std::endl;
            }
        }
        // If the stream ended early, don't leave the writer waiting for replies.
        limiter.abort();
        return n_reads;
    }

    /**
     * @brief Read batches of sequences from a file into the queue. The queue
     *        is closed when the file is exhausted or the writer has stopped.
     * @return the number of sequences read
     */
    uint64_t FastBatcher(
            const std::string &sequence_file,
            ThreadSafeQueue<SequenceBatch> *batches_queue,
            std::atomic<bool> &read_failed) {
        uint64_t n_seqs = 0;
        try {
            FastReader reader = FastReader(sequence_file);

            std::cerr << "Reading sequences from file: " << sequence_file << std::endl;
            while (true) {
                SequenceBatch seqs;
                if (reader.read(seqs, FASTQ_BATCH_SIZE) <= 0) {
                    break;
                }
                n_seqs += seqs.size();
                // blocks while the queue is full, fails if the writer has stopped
                if (!batches_queue->push(std::move(seqs))) {
                    break;
                }
            }
        }
        catch (const std::exception &ex) {
            std::cerr << "Failed to read sequences from file: " << sequence_file
                      << ": " << ex.what() << std::endl;
            read_failed = true;
        }
        batches_queue->close();
        return n_seqs;
    }


private:

    // The gRPC service stub for the service defined in Kraken2.proto
    std::unique_ptr<kraken2proto::Kraken2Service::Stub> sequence_stub;

    int WaitForServer() {
        // wait for server
        while (true) {
            ClientContext context;
            Kraken2ReadyRequest req;
            Kraken2ReadyResult response;
            Status status;
            try {
                status = sequence_stub->ServerReady(&context, req, &response);
                if (status.ok())
                {
                    std::cerr << "Server responded as ready." << std::endl;
                    break;
                }
            }
            // complete failure
            catch (const std::exception &ex) {
                std::cerr << "Server status check failed: "
                          << ex.what() << std::endl;
                return EX_UNAVAILABLE;
            }
            // not ready condition
            if (status.error_code() == grpc::StatusCode::UNAVAILABLE) {
                // server may come back
                std::cerr << "Server is not ready: " << status.error_message() << std::endl;
                std::cerr << "Waiting 2s..." << std::endl;
                std::this_thread::sleep_for(2s);
            }
            // unknown error
            else {
                std::cerr << "Server is in error state: "
                          << status.error_message() << std::endl;
                return status.error_code();
            }
        }
        return EX_OK;
    }

    void PrintSummary(const std::string &summary, const std::string &report_file) {
        if (report_file != "") {
            try {
                std::ofstream summary_file(report_file, std::ofstream::out);
                summary_file << summary;
                summary_file.close();
            }
            catch (const std::exception &ex) {
                std::cerr << "Failed to write report file"
                          << ": " << ex.what() << std::endl;
            }
        }
    }

    /**
     * @brief Print the classification.
     *
     * @param classification
     */
    void PrintClassification(const Kraken2SequenceResult &classification) {
        std::string classified = classification.classified() ? "C" : "U";
        std::cout
            << classified << '\t'
            << classification.id() << '\t'
            << classification.tax_id() << '\t'
            << classification.size() << '\t'
            << classification.hitlist() << '\n';
    }
};

void Usage(std::ostream &out) {
    out << "Usage: kraken2_client [options]" << std::endl
        << std::endl
        << "\t-h, -H, --help               Usage" << std::endl
        << "\t-s, -S, --sequence [path]    Path to sequence file (*.fast(a|q)(.gz), or - for stdin)" << std::endl
        << "\t-r, -R  --report   [path]    Path to output report file" << std::endl
        << "\t-i, -I  --host-ip            Server IP address (default: localhost)." << std::endl
        << "\t-p, -P, --port [num]         Server port (default: 8080)." << std::endl
        << "\t-k, -K, --shutdown           Shutdown server" << std::endl
        << std::endl
        << "Leave sequence blank to request the total summary data from the specified endpoint." << std::endl
        << std::endl;
}

[[noreturn]] void UsageError(const std::string &message) {
    std::cerr << "kraken2_client: " << message << std::endl << std::endl;
    Usage(std::cerr);
    exit(EX_USAGE);
}

void ParseCommandLine(int argc, char **argv, Options &opts) {
    // Long options map onto the short options. Upper case short options are
    // also accepted and mean the same as lower case.
    struct option long_options[] =
        {
            {"sequence", required_argument, NULL, 's'},
            {"report", required_argument, NULL, 'r'},
            {"host-ip", required_argument, NULL, 'i'},
            {"port", required_argument, NULL, 'p'},
            {"shutdown", no_argument, NULL, 'k'},
            {"help", no_argument, NULL, 'h'},
            {NULL, 0, NULL, 0}};
    const char *short_options = "hHs:S:r:R:i:I:p:P:kK";

    int opt;
    while ((opt = getopt_long(argc, argv, short_options, long_options, NULL)) != -1) {
        switch (tolower(opt))
        {
        case 'h':
            Usage(std::cout);
            exit(EX_OK);
        case 's':
            opts.sequence = optarg;
            break;
        case 'r':
            opts.report_file = optarg;
            break;
        case 'k':
            opts.shutdown = true;
            break;
        case 'i':
            opts.host = optarg;
            break;
        case 'p': {
            char *end = nullptr;
            long port = strtol(optarg, &end, 10);
            if (end == optarg || *end != '\0' || port < 0 || port > 65535) {
                UsageError(std::string("port number not valid (0 - 65535): ") + optarg);
            }
            opts.port = static_cast<int>(port);
            break;
        }
        case '?':
            if (optopt == '?') {  // asked for with -?
                Usage(std::cout);
                exit(EX_OK);
            }
            // getopt has already described the problem
            std::cerr << std::endl;
            Usage(std::cerr);
            exit(EX_USAGE);
        }
    }
    if (optind < argc) {
        UsageError(std::string("unexpected argument: ") + argv[optind]);
    }
}

int main(int argc, char **argv) {
    Options opts;
    ParseCommandLine(argc, argv, opts);

    int rtn_code = 0;
    std::string server_address = opts.host + ":" + std::to_string(opts.port);

    std::cerr << "Connecting to server: " << server_address << "." << std::endl;

    // when we send messages from the client we break up sequence
    // batches to stay below 128Mb message size as set up in kraken2_server.cc
    // We need to similarly ensure the the client can receive more than the
    // default 4MB message size. Just set it to the max
    grpc::ChannelArguments ch_args;
    ch_args.SetMaxReceiveMessageSize(INT_MAX);
    std::shared_ptr<grpc::Channel> ch =
        grpc::CreateCustomChannel(
            server_address,
            grpc::InsecureChannelCredentials(), ch_args);
    SequenceClient client(ch);

    if (opts.shutdown) {
        rtn_code = client.ShutdownServer();
    }
    else if (opts.sequence.empty()) {
        rtn_code = client.GetSummary();
    }
    else {
        const std::string filename(opts.sequence);
        const std::string report_file(opts.report_file);
        rtn_code = client.ClassifySequences(filename, report_file);
    }

    std::cerr << "Return code: " << rtn_code << std::endl;
    return rtn_code;
}
