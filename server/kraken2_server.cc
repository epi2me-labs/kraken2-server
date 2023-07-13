#include <getopt.h>
#include <csignal>

#include <grpc/grpc.h>
#include <grpc++/server.h>
#include <grpc++/server_builder.h>
#include <grpc++/server_context.h>
#include <grpc++/security/server_credentials.h>

#include "messages.h"
#include "classify_server.h"

using grpc::ResourceQuota;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::ServerReaderWriter;
using grpc::Status;
using grpc::StatusCode;

using kraken2proto::Kraken2ReadyRequest;
using kraken2proto::Kraken2ReadyResult;
using kraken2proto::Kraken2SummaryRequest;
using kraken2proto::Kraken2SummaryResults;
using kraken2proto::Kraken2ShutdownRequest;
using kraken2proto::Kraken2ShutdownResult;
using kraken2proto::Kraken2SequenceRequest;
using kraken2proto::Kraken2SequenceStreamResult;
using kraken2proto::Kraken2Service;



class ServiceImpl final : public Kraken2Service::Service {

public:
    ServiceImpl(Options opts, Kraken2ServerClassifier *classifier, std::promise<void> *exit_requested)
    : options(opts), classifier(classifier), exit_requested(exit_requested)
    {}

    /**
     * @brief Endpoint to request a summary of the classification history on the server.
     */
    Status GetSummary(
           ServerContext *context, const Kraken2SummaryRequest *req,
            Kraken2SummaryResults *results) override {
        if (!classifier->index_available) {
            return IndexStatus();
        }

        // Only return summary if the server is recording history.
        if (options.stats) {
            results->set_summary(classifier->GetSummary());
        }
        // Else indicate to the user it is not available.
        else {
            results->set_summary("Summary not available on this server.");
        }
        return Status::OK;
    }

    /** 
     * @brief Endpoint to request ask server if it is ready.
     */
    Status ServerReady(
            ServerContext *context, const Kraken2ReadyRequest *req,
            Kraken2ReadyResult *results) override {
        results->set_ready(classifier->index_available && !classifier->index_broken);
        return IndexStatus();
    }

    /**
     * @brief Endpoint to initiate a remote shutdown of the server.
     */
    Status RemoteShutdown(
            ServerContext *context, const Kraken2ShutdownRequest *req,
            kraken2proto::Kraken2ShutdownResult *result) override {
        std::cerr << "Received shutdown request." << std::endl;
        try {
            exit_requested->set_value();
            result->set_successful(true);
            std::cerr << "Shutdown request made." << std::endl;
        }
        catch (const std::exception &ex) {
            result->set_successful(false);
            std::cerr << "Failed to request shutdown"
                      << ": " << ex.what() << std::endl;
        }
        return Status::OK;
    }

    /**
     * @brief Endpoint to classify a stream of sequences and return
     *        a stream of classifications as response.
     */
    Status ClassifyStream(
            ServerContext *context, ServerStream *reader_writer) override {
        if (!classifier->index_available) {
            return IndexStatus();
        }

        std::string results;

        classifier->ProcessSequenceStream(context, reader_writer, std::ref(results));

        // If connection is open, send a final message containing the summary.
        if (!context->IsCancelled()) {
            Kraken2SequenceStreamResult summary;
            summary.set_summary(results.c_str());
            reader_writer->Write(summary);
        }

        return Status::OK;
    }

private:
    Options options;
    Kraken2ServerClassifier *classifier;
    std::promise<void> *exit_requested;

    grpc::Status IndexNotLoaded = grpc::Status(grpc::StatusCode::UNAVAILABLE, "Index not loaded yet, please wait.");
    grpc::Status IndexError = grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "There was an error loading the index, the server will remain unavailable without intervention.");
    grpc::Status IndexLoaded = grpc::Status(grpc::StatusCode::OK, "Index loaded.");

    grpc::Status IndexStatus(){
        if(!classifier->index_available) {
            return classifier->index_broken ? IndexError : IndexNotLoaded;
        }
        return IndexLoaded;
    } 

};

// This is used in a lambda below and passed to std::signal, for which we
// need a void(*)(int) 
std::promise<void> *exit_requested;

void RunServer(Options opts, Kraken2ServerClassifier *classifier) {
    std::string server_address = opts.host + ":" + std::to_string(opts.port);
    ServiceImpl service(opts, classifier, exit_requested);
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
    // Shared pointer so graceful shutdown can be invoked.
    std::shared_ptr<Server> server(builder.BuildAndStart());
    if (server == nullptr) {
        std::cout << "Failed to start server on " << server_address
                  << ". See above for more details." << std::endl;     
    } else {
        std::cout << "Server listening on " << server_address
                  << ". Press Ctrl-C to end." << std::endl;
        // handle interrupts
        auto handler = [](int s) { exit_requested->set_value(); };
        // TODO: what's the actual behaviour here, i.e. what does Shutdown() do?
        std::signal(SIGINT, handler);
        std::signal(SIGTERM, handler);
        std::signal(SIGQUIT, handler);

        // block until exit request is set
        auto f = exit_requested->get_future();
        f.wait();
        server->Shutdown();
    }
}


void Usage(int exit_code) {
    std::cerr << "Usage: kraken2-server [options]" << std::endl
              << std::endl
              << "Options: (* mandatory)" << std::endl
              << "\t-h, -H, -?, --help              Usage" << std::endl
              << "*\t-d, -D, --db [path]            Path to Kraken 2 database" << std::endl
              << "\t-r, -R, --max-requests [int]    Max number of requests from clients to process concurrently (0 for default)" << std::endl
              << "\t-x, -X, --thread-pool [int]     Number of threads to use to classify reads from each client." << std::endl
              << "\t-s, -S, --no-stats              Do not track statistics of all processed sequences on this server. Saves memory long-term." << std::endl
              << "\t-i, -I  --host-ip               Server IP address (default: localhost)." << std::endl
              << "\t-p, -P, --port [int]            Port number on which to listen for requests (0 - 65535, default 8080.)" << std::endl
              << "\t-k, -K, --report-kmer           Include distinct k-mers in reports" << std::endl
              << "\t-z, -Z, --report-zero           Include zero count taxons in reports" << std::endl
              << "\t-t, -T, --translated-search     Use translated search when running classifications" << std::endl
              << "\t-c, -C, --confidence [double]   Confidence score threshold (default: 0.0) (0 - 1)" << std::endl
              << "\t-q, -Q, --min-quality [int]     Minimum base quality used in classification (default: 0), only effective with FASTQ input)." << std::endl
              << "\t-g, -G, --hit-groups [int]      Minimum number of hit groups (overlapping k-mers sharing the same minimizer) needed to make a call (default: 2)" << std::endl
              << "\t-o, -O, --memory-mapping        Avoids loading database into RAM" << std::endl;
    exit(exit_code);
}


void ParseCommandLine(int argc, char **argv, Options &opts) {
    // Define the long shell arguments
    struct option long_options[] = {
        {"db", required_argument, NULL, 'd'},
        {"db", required_argument, NULL, 'D'},
        {"max-requests", required_argument, NULL, 'r'},
        {"max-requests", required_argument, NULL, 'R'},
        {"thread-pool", required_argument, NULL, 'x'},
        {"thread-pool", required_argument, NULL, 'X'},
        {"no-stats", no_argument, NULL, 's'},
        {"no-stats", no_argument, NULL, 'S'},
        {"host-ip", required_argument, NULL, 'i'},
        {"host-ip", required_argument, NULL, 'I'},
        {"port", required_argument, NULL, 'p'},
        {"port", required_argument, NULL, 'P'},
        {"report-kmer", no_argument, NULL, 'k'},
        {"report-kmer", no_argument, NULL, 'K'},
        {"report-zero", no_argument, NULL, 'z'},
        {"report-zero", no_argument, NULL, 'Z'},
        {"translated-search", no_argument, NULL, 't'},
        {"translated-search", no_argument, NULL, 'T'},
        {"confidence", no_argument, NULL, 'c'},
        {"confidence", no_argument, NULL, 'C'},
        {"min-quality", no_argument, NULL, 'q'},
        {"min-quality", no_argument, NULL, 'Q'},
        {"hit-groups", no_argument, NULL, 'g'},
        {"hit-groups", no_argument, NULL, 'G'},
        {"memory-mapping", no_argument, NULL, 'o'},
        {"memory-mapping", no_argument, NULL, 'O'},
        {"wait", required_argument, NULL, 'w'},
        {"wait", required_argument, NULL, 'W'},
        {"help", no_argument, NULL, 'h'},
        {"help", no_argument, NULL, 'H'},
        {NULL, 0, NULL, 0}
    };
    int opt;
    // Handle the various shell arguments (long mapped to short)
    while ((opt = getopt_long(
        argc, argv, "hH?d:D:r:R:sSkKzZc:C:q:Q:g:G:oOx:X:p:P:wW", long_options, NULL)) != -1) {
        switch (opt){
            case '?':
            case 'h':
            case 'H':
                Usage(0);
                break;
            case 'd':
            case 'D':
                opts.db_path = optarg;
                opts.taxonomy_filename = optarg + std::string("/taxo.k2d");
                opts.options_filename = optarg + std::string("/opts.k2d");
                opts.index_filename = optarg + std::string("/hash.k2d");
                break;
            case 'r':
            case 'R':
                opts.max_queue = atoi(optarg);
                if (opts.max_queue < 0) {
                    std::cerr << "Number of maximum concurrent requests cannot be less than 1 (0 for default)."
                              << std::endl;
                    exit(0);
                }
                break;
            case 'x':
            case 'X':
                opts.thread_pool = atoi(optarg);
                if (opts.thread_pool < 0) {
                    std::cerr << "Number of maximum threads per client cannot be less than 1."
                              << std::endl;
                    exit(0);
                }
                break;
            case 's':
            case 'S':
                opts.stats = false;
                break;
            case 'i':
            case 'I':
                opts.host = optarg;
                break;
            case 'p':
            case 'P':
                opts.port = atoi(optarg);
                if (opts.port < 0 || opts.port > 65535) {
                    std::cerr << "Port number not valid (0 - 65535)" << std::endl;
                    exit(0);
                }
                break;
            case 'k':
            case 'K':
                opts.report_kmer_data = true;
                break;
            case 'z':
            case 'Z':
                opts.report_zero_counts = true;
                break;
            case 't':
            case 'T':
                opts.use_translated_search = true;
                break;
            case 'c':
            case 'C':
                opts.confidence_threshold = atof(optarg);
                if (opts.confidence_threshold < 0 || opts.confidence_threshold > 1) {
                    std::cerr << "Confidence threshold is not valid (0 - 1)" << std::endl;
                    exit(0);
                }
                break;
            case 'q':
            case 'Q':
                opts.minimum_quality_score = atoi(optarg);
                if (opts.minimum_quality_score < 0) {
                    std::cerr << "Minimum quality score is not valid (> 0)" << std::endl;
                    exit(0);
                }
                break;
            case 'g':
            case 'G':
                opts.minimum_hit_groups = atoi(optarg);
                if (opts.minimum_hit_groups < 0) {
                    std::cerr << "Minimum hit groups is not valid (> 0)" << std::endl;
                    exit(0);
                }
                break;
            case 'o':
            case 'O':
                opts.use_memory_mapping = true;
                break;
            case 'w':
            case 'W':
                opts.wait = atoi(optarg);
        }
    }
    if (opts.db_path.empty()) {
        std::cerr << "You must specify the path to the Kraken 2 database." << std::endl;
        Usage(0);
    }
}


int main(int argc, char **argv) {
    Options opts;
    ParseCommandLine(argc, argv, opts);
    Kraken2ServerClassifier *classifier = new Kraken2ServerClassifier(opts);
    exit_requested = new std::promise<void>;
    RunServer(opts, classifier);

    int rtn = classifier->index_available ? EX_OK : EX_IOERR;
    delete classifier;
    delete exit_requested;
    return rtn;
}
