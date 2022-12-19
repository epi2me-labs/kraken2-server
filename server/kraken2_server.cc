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
using kraken2proto::Kraken2SequenceResults;
using kraken2proto::Kraken2SequenceStreamResult;
using kraken2proto::Kraken2Service;

// Just some global state... :/
Kraken2ServerClassifier *classifier;
std::promise<void> *exit_requested;


class ServiceImpl final : public Kraken2Service::Service
{

public:
    ServiceImpl(Options opts, Kraken2ServerClassifier *classifier, std::promise<void> *exit_requested)
    : options(opts), classifier(classifier), exit_requested(exit_requested)
    {}


    /**
     * @brief Endpoint to request a summary of the classification history on the server.
     *
     * @param context
     * @param req
     * @param results
     * @return Status
     */
    Status GetSummary(
        ServerContext *context,
        const Kraken2SummaryRequest *req,
        Kraken2SummaryResults *results) override
    {
        if (!classifier->index_available)
        {
            return IndexStatus();
        }

        // Only return summary if the server is recording history.
        if (options.stats)
        {
            results->set_summary(classifier->GetSummary());
        }
        // Else indicate to the user it is not available.
        else
        {
            results->set_summary("Summary not available on this server.");
        }
        return Status::OK;
    }


    /** @brief Endpoint to request ask server if it is ready.
     *
     * @param context
     * @param req
     * @param results
     * @return Status
     */
    Status ServerReady(
        ServerContext *context,
        const Kraken2ReadyRequest *req,
        Kraken2ReadyResult *results) override
    {
        results->set_ready(classifier->index_available && !classifier->index_broken);
        return IndexStatus();
    }

    /**
     * @brief Endpoint to initiate a remote shutdown of the server.
     *
     * @param context
     * @param req
     * @param results
     * @return Status
     */
    Status RemoteShutdown(
        ServerContext *context,
        const Kraken2ShutdownRequest *req,
        kraken2proto::Kraken2ShutdownResult *result
    ) override
    {
        std::cerr << "Received shutdown request." << std::endl;
        try
        {
            exit_requested->set_value();
            result->set_successful(true);
            std::cerr << "Shutdown request made." << std::endl;
        }
        catch (const std::exception &ex)
        {
            result->set_successful(false);
            std::cerr << "Failed to request shutdown"
                      << ": " << ex.what() << std::endl;
        }
        return Status::OK;
    }

    /**
     * @brief Endpoint to classify a batch of sequences and return a unary response.
     *
     * @param context
     * @param reader_writer
     * @param response
     * @return Status
     */
    Status ClassifyBatch(
        ServerContext *context,
        ServerReader<Kraken2SequenceRequest> *reader_writer,
        Kraken2SequenceResults *response) override
    {
        if (!classifier->index_available)
        {
            return IndexStatus();
        }

        // Read the sequeneces from the stream into a vector to be classified.
        Kraken2SequenceRequest req;
        std::vector<Sequence> seqs;
        while (reader_writer->Read(&req))
        {
            Sequence seq;
            // Convert sequence messages to Kraken2 Sequence objects.
            if (SequenceRequestToSequence(req, seq))
                seqs.push_back(std::move(seq));
        }

        // Classify the batch.
        std::string summary;
        std::map<string, Kraken2SequenceResult> classifications;
        classifier->ProcessBatch(seqs, summary, classifications);

        // Set the values of the response object which will be sent to the client.
        response->set_summary(summary);
        google::protobuf::Map<string, Kraken2SequenceResult> temp(classifications.begin(), classifications.end());
        *(response->mutable_classifications()) = temp;

        // End the request.
        return Status::OK;
    }

    /**
     * @brief Endpoint to classify a stream of sequences and return a a stream of classifications as response.
     *
     * @param context
     * @param reader_writer
     * @return Status
     */
    Status ClassifyStream(
        ServerContext *context,
        ServerReaderWriter<Kraken2SequenceStreamResult, Kraken2SequenceRequest> *reader_writer) override
    {
        if (!classifier->index_available)
        {
            return IndexStatus();
        }

        // Prepare the thread safe sequence and classification queues and promises to manage stream operation.
        Kraken2SequenceRequest req;
        ThreadSafeQueue<Sequence> *seqs = new ThreadSafeQueue<Sequence>();
        ThreadSafeQueue<Kraken2SequenceResult> *classifications = new ThreadSafeQueue<Kraken2SequenceResult>();
        // Promise and future objects to indicate:
        // • Reading thread has completed reading to this request handler thread
        // • Ending operation to the sequencing thread
        // • Ending operation to the writing thread
        std::promise<void> reads_complete, seq_end, write_end;
        std::future<void> seq_end_future = seq_end.get_future();
        std::future<void> write_end_future = write_end.get_future();
        std::future<void> rc_future = reads_complete.get_future();
        // Summary of classified sequences
        std::string results;

        // Create threads to read sequence stream from client, classify, and write classifications back to the client.
        std::thread read_thread(&ServiceImpl::ReadSequenceStream, this, seqs, reader_writer, std::move(reads_complete));
        std::thread sequencer_thread(&Kraken2ServerClassifier::ProcessSequenceStream, classifier, seqs, classifications, std::ref(results), std::move(seq_end_future));
        std::thread write_thread(&ServiceImpl::WriteSequenceStream, this, classifications, reader_writer, std::move(write_end_future));

        // Hold the stream open for as long as the client keeps their write stream open.
        rc_future.wait();
        read_thread.join();
        // Await the sequencing thread operating completely on the sequence queue while the connection remains open.
        while (seqs->size() > 0 && !context->IsCancelled())
        {
        }
        // Signal sequencing thread to end once queue is empty and await completing final duties
        seq_end.set_value();
        sequencer_thread.join();
        // Await the writing thread operating completely on the classification queue while the connection remains open.
        while (classifications->size() > 0 && !context->IsCancelled())
        {
        }
        // Signal writing thread to end once queue is empty and await completing final duties
        write_end.set_value();
        write_thread.join();

        delete seqs;
        delete classifications;

        // If connection is open, send a final message containing the summary.
        if (!context->IsCancelled())
        {
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

    grpc::Status IndexStatus()
    {
        if(!classifier->index_available)
        {
            return classifier->index_broken ? IndexError : IndexNotLoaded;
        }
        return IndexLoaded;
    } 

    /**
     * @brief Member function to be executed as its own thread. Reads from the given connection until client indicates it is done. Indicates via the given promise when it is complete.
     *
     * @param seqs
     * @param reader_writer
     * @param context
     * @param reads_complete
     */
    void ReadSequenceStream(
        ThreadSafeQueue<Sequence> *seqs,
        ServerReaderWriter<Kraken2SequenceStreamResult, Kraken2SequenceRequest> *reader_writer,
        std::promise<void> reads_complete)
    {
        Kraken2SequenceRequest req;
        // While the connection remains open and the client has not indicated it is done, read sequences
        while (reader_writer->Read(&req))
        {
            Sequence seq;
            if (SequenceRequestToSequence(req, seq))
                seqs->push(std::move(seq));
        }
        reads_complete.set_value();
    }

    /**
     * @brief Member function to be executed as its own thread. Writes classifications to the client with the given connection until the future resolves or connection closes.
     *
     * @param classifications
     * @param reader_writer
     * @param context
     * @param end
     */
    void WriteSequenceStream(
        ThreadSafeQueue<Kraken2SequenceResult> *classifications,
        ServerReaderWriter<Kraken2SequenceStreamResult, Kraken2SequenceRequest> *reader_writer,
        std::future<void> end)
    {
        // While the future is unresolved and connection is open, if the queue contains anything, write to the client.
        while (end.wait_for(std::chrono::seconds(0)) == std::future_status::timeout)
        {
            std::optional<Kraken2SequenceResult> c = classifications->pop();
            if (c.has_value())
            {
                Kraken2SequenceStreamResult result;
                *(result.mutable_classification()) = c.value();
                reader_writer->Write(result);
            }
        }
    }
};


void RunServer(Options opts)
{
    std::string server_address = opts.host + ":" + std::to_string(opts.port);
    ServiceImpl service(opts, classifier, exit_requested);
    // Sets the max number of concurrent requests
    ResourceQuota rq;
    if (opts.max_queue > 0)
    {
        // +1 to account for the server thread itself. If one thread were to be specified, all requests are rejected.
        rq.SetMaxThreads(opts.max_queue + 1);
    }
    // According to gRPC devs, does not fully track gRPC memory usage and no docs on unit of memory allocation - not recommended
    // rq.Resize(new_memory_allocation);
    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.AddChannelArgument(GRPC_ARG_ALLOW_REUSEPORT, 0);  // don't use port if already in use
    builder.SetResourceQuota(rq);
    builder.RegisterService(&service);
    // Shared pointer so graceful shutdown can be invoked.
    std::shared_ptr<Server> server(builder.BuildAndStart());
    if (server == nullptr) {
        std::cout << "Failed to start server on " << server_address << ". See above for more details." << std::endl;
        
    } else {
        std::cout << "Server listening on " << server_address << ". Press Ctrl-C to end." << std::endl;

        // handle interrupts
        auto handler = [](int s) {
          exit_requested->set_value();
        };
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

void Usage(int exit_code)
{
    std::cerr << "Usage: kraken2-server [options]" << std::endl
              << std::endl
              << "Options: (* mandatory)" << std::endl
              << "\t-h, -H, -?, --help              Usage" << std::endl
              << "*\t-d, -D, --db [path]            Path to Kraken 2 database" << std::endl
              << "\t-r, -R, --max-requests [int]    Max number of requests from clients to process concurrently (0 for default)" << std::endl
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

void ParseCommandLine(int argc, char **argv, Options &opts)
{
    // Define the long shell arguments
    struct option long_options[] =
        {
            {"db", required_argument, NULL, 'd'},
            {"db", required_argument, NULL, 'D'},
            {"max-requests", required_argument, NULL, 'r'},
            {"max-requests", required_argument, NULL, 'R'},
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
            {NULL, 0, NULL, 0}};
    int opt;
    // Handle the various shell arguments (long mapped to short)
    while ((opt = getopt_long(argc, argv, "hH?d:D:r:R:sSkKzZc:C:q:Q:g:G:oOx:X:p:P:wW", long_options, NULL)) != -1)
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
            opts.taxonomy_filename = optarg + std::string("/taxo.k2d");
            opts.options_filename = optarg + std::string("/opts.k2d");
            opts.index_filename = optarg + std::string("/hash.k2d");
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
        case 'i':
        case 'I':
            opts.host = optarg;
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
        case 'w':
        case 'W':
            opts.wait = atoi(optarg);
        }
    }
    if (opts.db_path.empty())
    {
        std::cerr << "You must specify the path to the Kraken 2 database." << std::endl;
        Usage(0);
    }
}


int main(int argc, char **argv)
{
    Options opts;
    ParseCommandLine(argc, argv, opts);
    classifier = new Kraken2ServerClassifier(opts);
    exit_requested = new std::promise<void>;
    RunServer(opts);

    return classifier->index_available ? EX_OK : EX_IOERR;
}
