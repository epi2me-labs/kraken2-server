#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <getopt.h>
#include <mutex>
#include <pthread.h>
#include <sysexits.h>
#include <unistd.h>

#include <grpc/grpc.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>
#include <grpcpp/security/server_credentials.h>

#include "messages.h"
#include "classify_server.h"

using grpc::ResourceQuota;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using grpc::StatusCode;

using kraken2proto::Kraken2ReadyRequest;
using kraken2proto::Kraken2ReadyResult;
using kraken2proto::Kraken2SummaryRequest;
using kraken2proto::Kraken2SummaryResults;
using kraken2proto::Kraken2ShutdownRequest;
using kraken2proto::Kraken2ShutdownResult;
using kraken2proto::Kraken2SequenceStreamResult;
using kraken2proto::Kraken2Service;


/**
 * @brief A one-way latch used to ask the server to stop. Requesting is
 *        idempotent and may come from several places (signals, remote
 *        shutdown request).
 */
class ShutdownLatch {
public:
    void Request() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            requested_ = true;
        }
        cv_.notify_all();
    }

    void Wait() {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait(lock, [this] { return requested_; });
    }

private:
    std::mutex mtx_;
    std::condition_variable cv_;
    bool requested_ = false;
};


class ServiceImpl final : public Kraken2Service::Service {

public:
    ServiceImpl(const Options &opts, Kraken2ServerClassifier &classifier, ShutdownLatch &shutdown)
    : options(opts), classifier(classifier), shutdown(shutdown)
    {}

    /**
     * @brief Endpoint to request a summary of the classification history on the server.
     */
    Status GetSummary(
           ServerContext *context, const Kraken2SummaryRequest *req,
            Kraken2SummaryResults *results) override {
        Status status = IndexStatus();
        if (!status.ok()) {
            return status;
        }

        // Only return summary if the server is recording history.
        if (options.stats) {
            results->set_summary(classifier.GetSummary());
        }
        // Else indicate to the user it is not available.
        else {
            results->set_summary("Summary not available on this server.");
        }
        return Status::OK;
    }

    /**
     * @brief Endpoint to ask the server if it is ready. Clients use the status
     *        to tell "still loading" (UNAVAILABLE) from "broken".
     */
    Status ServerReady(
            ServerContext *context, const Kraken2ReadyRequest *req,
            Kraken2ReadyResult *results) override {
        results->set_ready(classifier.state() == IndexState::Ready);
        return IndexStatus();
    }

    /**
     * @brief Endpoint to initiate a remote shutdown of the server.
     */
    Status RemoteShutdown(
            ServerContext *context, const Kraken2ShutdownRequest *req,
            Kraken2ShutdownResult *result) override {
        std::cerr << "Received shutdown request." << std::endl;
        shutdown.Request();
        result->set_successful(true);
        return Status::OK;
    }

    /**
     * @brief Endpoint to classify a stream of sequences and return
     *        a stream of classifications as response.
     */
    Status ClassifyStream(
            ServerContext *context, ServerStream *reader_writer) override {
        Status status = IndexStatus();
        if (!status.ok()) {
            return status;
        }

        std::string report;
        status = classifier.ProcessSequenceStream(context, reader_writer, report);
        if (!status.ok()) {
            return status;
        }

        // If connection is open, send a final message containing the report.
        if (!context->IsCancelled()) {
            Kraken2SequenceStreamResult summary;
            summary.set_summary(report);
            reader_writer->Write(summary);
        }

        return Status::OK;
    }

private:
    const Options &options;
    Kraken2ServerClassifier &classifier;
    ShutdownLatch &shutdown;

    Status IndexStatus() const {
        switch (classifier.state()) {
            case IndexState::Ready:
                return Status::OK;
            case IndexState::Failed:
                return Status(StatusCode::FAILED_PRECONDITION,
                    "There was an error loading the index, the server will remain unavailable without intervention.");
            case IndexState::Loading:
            default:
                return Status(StatusCode::UNAVAILABLE, "Index not loaded yet, please wait.");
        }
    }
};


/**
 * @brief Turns SIGINT/SIGTERM/SIGQUIT into a request to shut down.
 *
 * The signals are blocked in the calling thread (and so in every thread created
 * after it), and a dedicated thread waits for them synchronously. That avoids
 * doing anything but the most trivial work in an asynchronous signal handler.
 * Construct this before creating any other threads.
 */
class SignalWatcher {
public:
    explicit SignalWatcher(ShutdownLatch &shutdown) {
        sigemptyset(&set_);
        sigaddset(&set_, SIGINT);
        sigaddset(&set_, SIGTERM);
        sigaddset(&set_, SIGQUIT);
        // A shell starts background jobs with SIGINT ignored, and ignored
        // signals are never delivered to sigwait.
        for (int sig : {SIGINT, SIGTERM, SIGQUIT}) {
            signal(sig, SIG_DFL);
        }
        pthread_sigmask(SIG_BLOCK, &set_, nullptr);
        thread_ = std::thread([this, &shutdown]() {
            int sig;
            if (sigwait(&set_, &sig) == 0) {
                if (!stopping_) {
                    std::cerr << "Received signal " << sig << "." << std::endl;
                }
                shutdown.Request();
            }
            done_ = true;
        });
    }

    ~SignalWatcher() {
        // wake the watcher thread if no signal ever arrived
        stopping_ = true;
        if (!done_) {
            pthread_kill(thread_.native_handle(), SIGTERM);
        }
        thread_.join();
    }

private:
    sigset_t set_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> done_{false};
    std::thread thread_;
};


/**
 * @brief Serve requests until a shutdown is requested.
 * @return EX_OK on a clean shutdown, EX_UNAVAILABLE if the server could not be started.
 */
int RunServer(const Options &opts, Kraken2ServerClassifier &classifier, ShutdownLatch &shutdown) {
    std::string server_address = opts.host + ":" + std::to_string(opts.port);
    ServiceImpl service(opts, classifier, shutdown);
    // Sets the max number of concurrent requests
    ResourceQuota rq;
    if (opts.max_queue > 0){
        // +1 to account for the server thread itself. If one thread were to
        // be specified, all requests are rejected.
        rq.SetMaxThreads(opts.max_queue + 1);
    }
    // According to gRPC devs, does not fully track gRPC memory usage and no
    // docs on unit of memory allocation - not recommended
    // rq.Resize(new_memory_allocation);
    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    // don't use port if already in use
    builder.AddChannelArgument(GRPC_ARG_ALLOW_REUSEPORT, 0);
    builder.SetResourceQuota(rq);
    builder.RegisterService(&service);
    // allow 128Mb messages
    builder.SetMaxSendMessageSize(128 * 1024 * 1024);
    builder.SetMaxMessageSize(128 * 1024 * 1024);
    builder.SetMaxReceiveMessageSize(128 * 1024 * 1024);
    std::unique_ptr<Server> server(builder.BuildAndStart());
    if (server == nullptr) {
        std::cerr << "Failed to start server on " << server_address
                  << ". See above for more details." << std::endl;
        return EX_UNAVAILABLE;
    }
    std::cout << "Server listening on " << server_address
              << ". Press Ctrl-C to end." << std::endl;

    shutdown.Wait();
    std::cerr << "Shutting down." << std::endl;
    // Lets in-flight calls finish, but gives up on them after a while so a
    // stalled client cannot keep the server alive indefinitely.
    server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(10));
    return EX_OK;
}


void Usage(std::ostream &out) {
    out << "Usage: kraken2_server [options]" << std::endl
        << std::endl
        << "Options: (* mandatory)" << std::endl
        << "\t-h, -H, --help                  Usage" << std::endl
        << "*\t-d, -D, --db [path]            Path to Kraken 2 database" << std::endl
        << "\t-r, -R, --max-requests [int]    Max number of requests from clients to process concurrently (0 for default)" << std::endl
        << "\t-x, -X, --thread-pool [int]     Number of threads used to classify reads, shared by all clients (default: 1, 0 for all hardware threads)" << std::endl
        << "\t-s, -S, --no-stats              Do not track statistics of all processed sequences on this server. Saves memory long-term." << std::endl
        << "\t-i, -I  --host-ip               Server IP address (default: localhost)." << std::endl
        << "\t-p, -P, --port [int]            Port number on which to listen for requests (0 - 65535, default 8080.)" << std::endl
        << "\t-k, -K, --report-kmer           Include distinct k-mers in reports" << std::endl
        << "\t-z, -Z, --report-zero           Include zero count taxons in reports" << std::endl
        << "\t-t, -T, --translated-search     Has no effect: translated search is used if the database was built for it" << std::endl
        << "\t-c, -C, --confidence [double]   Confidence score threshold (default: 0.0) (0 - 1)" << std::endl
        << "\t-q, -Q, --min-quality [int]     Minimum base quality used in classification (default: 0), only effective with FASTQ input)." << std::endl
        << "\t-g, -G, --hit-groups [int]      Minimum number of hit groups (overlapping k-mers sharing the same minimizer) needed to make a call (default: 2)" << std::endl
        << "\t-o, -O, --memory-mapping        Avoids loading database into RAM" << std::endl
        << "\t-w, -W, --wait [int]            Seconds to wait before loading the database (for testing clients)" << std::endl;
}


[[noreturn]] void UsageError(const std::string &message) {
    std::cerr << "kraken2_server: " << message << std::endl << std::endl;
    Usage(std::cerr);
    exit(EX_USAGE);
}


// Parse an integer option value, insisting it is a whole number in [min, max].
int ParseInt(const char *value, const char *name, long min, long max) {
    char *end = nullptr;
    errno = 0;
    long parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0' || errno != 0 || parsed < min || parsed > max) {
        UsageError(std::string("invalid value for ") + name + ": '" + value + "' (expected " +
                   std::to_string(min) + " - " + std::to_string(max) + ")");
    }
    return static_cast<int>(parsed);
}


double ParseDouble(const char *value, const char *name, double min, double max) {
    char *end = nullptr;
    errno = 0;
    double parsed = strtod(value, &end);
    if (end == value || *end != '\0' || errno != 0 || !(parsed >= min && parsed <= max)) {
        UsageError(std::string("invalid value for ") + name + ": '" + value + "' (expected " +
                   std::to_string(min) + " - " + std::to_string(max) + ")");
    }
    return parsed;
}


void ParseCommandLine(int argc, char **argv, Options &opts) {
    // Long options map onto the short options. Upper case short options are
    // also accepted and mean the same as lower case.
    struct option long_options[] = {
        {"db", required_argument, NULL, 'd'},
        {"max-requests", required_argument, NULL, 'r'},
        {"thread-pool", required_argument, NULL, 'x'},
        {"no-stats", no_argument, NULL, 's'},
        {"host-ip", required_argument, NULL, 'i'},
        {"port", required_argument, NULL, 'p'},
        {"report-kmer", no_argument, NULL, 'k'},
        {"report-zero", no_argument, NULL, 'z'},
        {"translated-search", no_argument, NULL, 't'},
        {"confidence", required_argument, NULL, 'c'},
        {"min-quality", required_argument, NULL, 'q'},
        {"hit-groups", required_argument, NULL, 'g'},
        {"memory-mapping", no_argument, NULL, 'o'},
        {"wait", required_argument, NULL, 'w'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    const char *short_options = "hHd:D:r:R:x:X:sSi:I:p:P:kKzZtTc:C:q:Q:g:G:oOw:W:";

    int opt;
    while ((opt = getopt_long(argc, argv, short_options, long_options, NULL)) != -1) {
        switch (tolower(opt)) {
            case 'h':
                Usage(std::cout);
                exit(EX_OK);
            case 'd':
                opts.db_path = optarg;
                opts.taxonomy_filename = opts.db_path + "/taxo.k2d";
                opts.options_filename = opts.db_path + "/opts.k2d";
                opts.index_filename = opts.db_path + "/hash.k2d";
                break;
            case 'r':
                opts.max_queue = ParseInt(optarg, "--max-requests", 0, INT_MAX);
                break;
            case 'x':
                opts.thread_pool = ParseInt(optarg, "--thread-pool", 0, 4096);
                break;
            case 's':
                opts.stats = false;
                break;
            case 'i':
                opts.host = optarg;
                break;
            case 'p':
                opts.port = ParseInt(optarg, "--port", 0, 65535);
                break;
            case 'k':
                opts.report_kmer_data = true;
                break;
            case 'z':
                opts.report_zero_counts = true;
                break;
            case 't':
                opts.use_translated_search = true;
                break;
            case 'c':
                opts.confidence_threshold = ParseDouble(optarg, "--confidence", 0.0, 1.0);
                break;
            case 'q':
                opts.minimum_quality_score = ParseInt(optarg, "--min-quality", 0, 100);
                break;
            case 'g':
                opts.minimum_hit_groups = ParseInt(optarg, "--hit-groups", 0, INT_MAX);
                break;
            case 'o':
                opts.use_memory_mapping = true;
                break;
            case 'w':
                opts.wait = ParseInt(optarg, "--wait", 0, INT_MAX);
                break;
            case '?':
                if (optopt == '?') {  // asked for with -?
                    Usage(std::cout);
                    exit(EX_OK);
                }
                // getopt has already described the problem
                std::cerr << std::endl;
                Usage(std::cerr);
                exit(EX_USAGE);
            default:
                // getopt has already described the problem
                std::cerr << std::endl;
                Usage(std::cerr);
                exit(EX_USAGE);
        }
    }
    if (optind < argc) {
        UsageError(std::string("unexpected argument: ") + argv[optind]);
    }
    if (opts.db_path.empty()) {
        UsageError("you must specify the path to the Kraken 2 database.");
    }
    // Fail now, rather than serving an unusable server, if the database is not there.
    for (const std::string &file : {opts.taxonomy_filename, opts.options_filename, opts.index_filename}) {
        if (access(file.c_str(), R_OK) != 0) {
            std::cerr << "kraken2_server: cannot read " << file << ": " << strerror(errno) << std::endl;
            exit(EX_NOINPUT);
        }
    }
}


int main(int argc, char **argv) {
    Options opts;
    ParseCommandLine(argc, argv, opts);

    ShutdownLatch shutdown;
    // Must come before any threads are started
    SignalWatcher signals(shutdown);

    Kraken2ServerClassifier classifier(opts);
    int rtn = RunServer(opts, classifier, shutdown);
    if (rtn == EX_OK && classifier.state() == IndexState::Failed) {
        rtn = EX_IOERR;
    }
    return rtn;
}
