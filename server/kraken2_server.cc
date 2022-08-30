#include <iostream>
#include <sstream>
#include <memory>
#include <string>
#include <map>
#include <cstdint>
#include <stdexcept>
#include <getopt.h>
#include <csignal>
#include <thread>
#include <vector>
#include <queue>

#include <grpc/grpc.h>
#include <grpc++/server.h>
#include <grpc++/server_builder.h>
#include <grpc++/server_context.h>
#include <grpc++/security/server_credentials.h>

#include "sequential_file_writer.h"
#include "messages.h"
#include "classify_server.h"

using grpc::ResourceQuota;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::Status;
using grpc::StatusCode;

using kraken2proto::Kraken2SequenceRequest;
using kraken2proto::Kraken2SequenceResults;
using kraken2proto::Kraken2Service;
using kraken2proto::Kraken2SummaryRequest;
using kraken2proto::Kraken2SummaryResults;

class ServiceImpl final : public Kraken2Service::Service
{

public:
    ServiceImpl(Options opts, Kraken2ServerClassifier *classifier) : options(opts), classifier(classifier) {}

    void PrintSequenceSample(std::vector<Sequence> &seqs)
    {
        int i = 0;
        std::cout << seqs.at(0).to_string() << std::endl;
        for (auto &s : seqs)
        {
            if (i++ % 250 == 0)
                std::cout << seqs.at(i).to_string() << std::endl;
        }
    }

    Status PutSequence(
        ServerContext *context,
        ServerReader<Kraken2SequenceRequest> *reader_writer,
        Kraken2SequenceResults *response) override
    {
        // Buffer to hold chunk of sequence stream.
        Kraken2SequenceRequest req;
        std::vector<Sequence> seqs;

        // While chunks are available from the SequenceContent stream.
        while (reader_writer->Read(&req))
        {
            Sequence seq;
            if (SequenceRequestToSequence(req, seq))
                seqs.push_back(std::move(seq));
        }
        return RespondPutSequence(seqs, response);
    }

    Status GetSummary(ServerContext *context,
                      const Kraken2SummaryRequest *req,
                      Kraken2SummaryResults *results) override
    {
        if (options.stats)
        {
            results->set_summary(classifier->GetSummary());
        }
        else
        {
            results->set_summary("Summary not available on this server.");
        }
        return Status::OK;
    }

private:
    Options options;
    Kraken2ServerClassifier *classifier;

    Status RespondPutSequence(std::vector<Sequence> &seqs, Kraken2SequenceResults *response)
    {
        std::string summary;
        std::map<string, Kraken2SequenceResult> classifications;
        // Acquire standard map of results
        classifier->ProcessSequence(seqs, summary, classifications);
        response->set_summary(summary);
        // Convert to protobuf map
        google::protobuf::Map<string, Kraken2SequenceResult> temp(classifications.begin(), classifications.end());
        *(response->mutable_classifications()) = temp;

        return Status::OK;
    }
};

bool shutdown_required = false;
std::vector<std::thread> threads;
Kraken2ServerClassifier *classifier;

void CheckShutdown(std::shared_ptr<Server> server)
{
    while (!shutdown_required)
        sleep(1);
    // Does not complete inflight requests while acting as a sync server, possibly requires async
    const std::chrono::time_point<std::chrono::system_clock> deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(10000);
    server->Shutdown(deadline);
}

void RunServer(Options opts)
{
    std::string server_address = "localhost:" + std::to_string(opts.port);
    ServiceImpl service(opts, classifier);
    ResourceQuota rq;
    if (opts.max_queue > 0)
    {
        // +1 to account for the server thread itself. If one thread were to be specified, all requests would be rejected.
        rq.SetMaxThreads(opts.max_queue + 1);
    }
    // According to gRPC devs, does not fully track gRPC memory usage and no docs on unit of memory allocation
    // rq.Resize(new_memory_allocation);
    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.SetResourceQuota(rq);
    builder.RegisterService(&service);
    std::shared_ptr<Server> server(builder.BuildAndStart());
    std::thread shutdown_thread(CheckShutdown, server);
    threads.push_back(std::move(shutdown_thread));
    std::cout << "Server listening on " << server_address << ". Press Ctrl-C to end." << std::endl;
    server->Wait();
}

void Usage(int exit_code)
{
    std::cerr << "Usage: kraken2-server [options]" << std::endl
              << std::endl
              << "Options: (* mandatory)" << std::endl
              << "\t-h, -H, -?, --help              Usage" << std::endl
              << "*\t-d, -D, --db [path]            Path to Kraken 2 database" << std::endl
              << "\t-r, -R, --max-requests [int]    Max number of requests from clients to process concurrently (0 for default)" << std::endl
              << "\t-s, -S, --no-stats              Do not track statistics of all processed sequences on this server. Saves memory long-term." << std::endl
              << "\t-p, -P, --port [int]            Port number on which to listen for requests (0 - 65535)" << std::endl
              << "\t-k, -K, --report-kmer           Include distinct k-mers in reports" << std::endl
              << "\t-z, -Z, --report-zero           Include zero count taxons in reports" << std::endl
              << "\t-t, -T, --translated-search     Use translated search when running classifications" << std::endl
              << "\t-c, -C, --confidence [double]   Confidence score threshold (default: 0.0) (0 - 1)" << std::endl
              << "\t-q, -Q, --min-quality [int]     Minimum base quality used in classification (default: 0), only effective with FASTQ input)." << std::endl
              << "\t-g, -G, --hit-groups [int]      Minimum number of hit groups (overlapping k-mers sharing the same minimizer) needed to make a call (default: 2)" << std::endl
              << "\t-o, -O, --memory-mapping        Avoids loading database into RAM" << std::endl;
    exit(exit_code);
}

void ParseCommandLine(int argc, char **argv, Options &opts)
{
    // Define the long shell arguments
    struct option long_options[] =
        {
            {"db", required_argument, NULL, 'd'},
            {"db", required_argument, NULL, 'D'},
            {"max-requests", required_argument, NULL, 'r'},
            {"max-requests", required_argument, NULL, 'R'},
            {"no-stats", required_argument, NULL, 's'},
            {"no-stats", required_argument, NULL, 'S'},
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
            {"help", no_argument, NULL, 'h'},
            {"help", no_argument, NULL, 'H'},
            {NULL, 0, NULL, 0}};
    int opt;
    // Handle the various shell arguments (long mapped to short)
    while ((opt = getopt_long(argc, argv, "hH?d:D:r:R:sSkKzZc:C:q:Q:g:G:oOp:P:", long_options, NULL)) != -1)
    {
        switch (opt)
        {
        case '?':
        case 'h':
        case 'H':
            Usage(0);
            break;
        case 'd':
        case 'D':
            opts.db_path = optarg;
            opts.taxonomy_filename = optarg + std::string("taxo.k2d");
            opts.options_filename = optarg + std::string("opts.k2d");
            opts.index_filename = optarg + std::string("hash.k2d");
            break;
        case 'r':
        case 'R':
            opts.max_queue = atoi(optarg);
            if (opts.max_queue < 0)
            {
                std::cerr << "Number of maximum concurrent requests cannot be less than 1 (0 for default)." << std::endl;
                exit(0);
            }
            break;
        case 's':
        case 'S':
            opts.stats = false;
            break;
        case 'p':
        case 'P':
            opts.port = atoi(optarg);
            if (opts.port < 0 || opts.port > 65535)
            {
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
            if (opts.confidence_threshold < 0 || opts.confidence_threshold > 1)
            {
                std::cerr << "Confidence threshold is not valid (0 - 1)" << std::endl;
                exit(0);
            }
            break;
        case 'q':
        case 'Q':
            opts.minimum_quality_score = atoi(optarg);
            if (opts.minimum_quality_score < 0)
            {
                std::cerr << "Minimum quality score is not valid (> 0)" << std::endl;
                exit(0);
            }
            break;
        case 'g':
        case 'G':
            opts.minimum_hit_groups = atoi(optarg);
            if (opts.minimum_hit_groups < 0)
            {
                std::cerr << "Minimum hit groups is not valid (> 0)" << std::endl;
                exit(0);
            }
            break;
        case 'o':
        case 'O':
            opts.use_translated_search = true;
            break;
        }
    }
    if (opts.db_path.empty())
    {
        std::cerr << "You must specify the path to the Kraken 2 database." << std::endl;
        Usage(0);
    }
}

void InitKraken(int argc, char **argv, Options &opts)
{
    classifier = new Kraken2ServerClassifier(argc, argv, opts);
    // Any additional Kraken2 initialisation
}

void Shutdown(int signum)
{
    std::cout << "Interrupt signal (" << signum << ") received." << std::endl;
    shutdown_required = true;
    exit(signum);
}

int main(int argc, char **argv)
{
    signal(SIGINT, Shutdown);
    Options opts;
    ParseCommandLine(argc, argv, opts);
    InitKraken(argc, argv, opts);

    RunServer(opts);

    // Join additional threads to ensure disposal before exitting
    for (std::thread &t : threads)
        t.join();
    return 0;
}