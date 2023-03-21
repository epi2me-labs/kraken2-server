#include <atomic>
#include <chrono>
#include <fstream>
#include <future>
#include <getopt.h>
#include <thread>
#include <sysexits.h>

#include <grpc/grpc.h>
#include <grpc++/channel.h>
#include <grpc++/client_context.h>
#include <grpc++/create_channel.h>
#include <grpc++/security/credentials.h>

#include "utils.h"
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


#define ST_BATCH_SIZE 100      // reads in a gRPC batch
#define MAX_IN_FLIGHT 40000    // total reads in gRPC system
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
     * @return EX_UNAVAILABLE if sequences could nto be sent to server
     * @return else gRPC status code
     */
    int ClassifySequences(const std::string &sequence_name, const std::string &report_file) {
            //std::vector<Kraken2SequenceRequest> &data) {
        std::cerr << "Classifying sequence stream." << std::endl;
        int state = WaitForServer();
        if (state != 0) {return state;}

        ClientContext context;
        Kraken2SequenceResultMulti response;
        ClientStream stream(sequence_stub->ClassifyStream(&context));

        // create a queue and thread to read sequences from file, and batch
        // into gRPC messages for the stream 
        ThreadSafeQueue<std::vector<Kraken2SequenceRequest>>
            *batches_queue = new ThreadSafeQueue<std::vector<Kraken2SequenceRequest>>();
        std::promise<void> complete;
        std::future<void> batches_complete = complete.get_future();
        std::thread batcher_thread(
            &SequenceClient::FastBatcher, this,
            std::ref(sequence_name), batches_queue);

        // create a writer and a reader thread, track number of sequences in flight
        std::atomic<uint64_t> seqs_in_flight = 0;
        auto write_future = std::async(
            std::launch::async, &SequenceClient::StreamWriter, this,
            std::move(batches_complete),
            std::ref(seqs_in_flight),
            batches_queue, stream);
        auto read_future = std::async(
            std::launch::async, &SequenceClient::StreamReader, this,
            std::ref(seqs_in_flight), std::ref(report_file), stream);

        // wait for all reads to be read;
        batcher_thread.join();
        complete.set_value();
        // wait for writing to finish
        write_future.wait();
        int write_rtn = write_future.get();
        if (write_rtn != EX_OK) {
            read_future.wait();
            return write_rtn;
        }

        // Handle the stream response
        Status status = stream->Finish();
        if (!status.ok()) {
            std::cerr << "Client RPC stream failed: " << status.error_message() << std::endl;
        }
        return status.error_code();

        delete batches_queue;
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

    int StreamWriter(
            std::future<void> finish,
            std::atomic<uint64_t> &seqs_in_flight,
            ThreadSafeQueue<std::vector<Kraken2SequenceRequest>> *batches,
            ClientStream writer) {
        try {
            while ( finish.wait_for(0s) == std::future_status::timeout || batches->size() > 0) {
                std::optional<std::vector<Kraken2SequenceRequest>> batch = batches->pop();
                if (batch.has_value()) {
                    while (true) {
                        if ((seqs_in_flight + batch->size() >= MAX_IN_FLIGHT)) {
                            std::this_thread::sleep_for(10ms);
                            std::cerr << "Waiting before sending more. In-flight: " << seqs_in_flight << "." << std::endl;
                            continue;
                        }
                        else { break; }
                    }

                    // rebatch to smaller batches for stream
                    size_t size = (batch->size() - 1) / ST_BATCH_SIZE + 1;
                    for (size_t k = 0; k < size; ++k) {
                        std::vector<Kraken2SequenceRequest> st_batch;
                        auto start_itr = std::next(batch->cbegin(), k*ST_BATCH_SIZE);
                        auto end_itr = std::next(batch->cbegin(), k*ST_BATCH_SIZE + ST_BATCH_SIZE);
                        st_batch.resize(ST_BATCH_SIZE);
                        if (k*ST_BATCH_SIZE + ST_BATCH_SIZE > batch->size()) {
                            end_itr = batch->cend();
                            st_batch.resize(st_batch.size() - k*ST_BATCH_SIZE);
                        }
                        std::move(start_itr, end_itr, st_batch.begin());
                        // create stream message and send
                        Kraken2SequenceRequestMulti req;
                        req.mutable_seqs()->Assign(st_batch.begin(), st_batch.end());
                        writer->Write(req, WriteOptions().set_buffer_hint());
                        seqs_in_flight.fetch_add(st_batch.size()); 
                    }
                }
            }
            writer->WritesDone();
        }
        catch (const std::exception &ex) {
            std::cerr << "Failed to send sequences"
                      << ": " << ex.what() << std::endl;
            return EX_UNAVAILABLE;
        }

        std::cerr << "Sent sequences." << std::endl;
        return EX_OK;
    }

    void StreamReader(
            std::atomic<uint64_t> &seqs_in_flight, const std::string &report_file,
            ClientStream reader) {
        Kraken2SequenceStreamResult result;
        while (reader->Read(&result)) {
            if (result.has_classifications()) {
                for (auto &res : result.classifications().classes()){
                    PrintClassification(res);
                    seqs_in_flight--;
                }
            }
        }
        if (result.has_summary())
            PrintSummary(result.summary(), report_file);
    }
    
    int FastBatcher(
            const std::string &sequence_file,
            ThreadSafeQueue<std::vector<Kraken2SequenceRequest>> *batches_queue) {
        FastReader reader = FastReader(sequence_file);
        
        std::cerr << "Reading sequences from file: " << sequence_file << std::endl;
        int total_reads = 0;
        while (true) {
            if ((batches_queue->size() >= MAX_BATCHES)) {
                std::cerr << "Waiting before sending more. In-flight: " << batches_queue->size() << "." << std::endl;
                std::this_thread::sleep_for(10ms);
                continue;
            }
 
            std::vector<Kraken2SequenceRequest> seqs;
            int n_reads;
            if ((n_reads = reader.read(seqs, FASTQ_BATCH_SIZE)) > 0) {
                total_reads += n_reads;
                batches_queue->push(std::move(seqs));
            }
            else { break; }
        }
        std::cerr << "Finished reading " << total_reads << " sequences." << std::endl;
        return total_reads;
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
                std::cerr << "Waiting 10s..." << std::endl;
                std::this_thread::sleep_for(10s);
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
    void PrintClassification(Kraken2SequenceResult classification) {
        std::string classified = classification.classified() ? "C" : "U";
        std::cout
            << classified << '\t'
            << classification.id() << '\t'
            << classification.tax_id() << '\t'
            << classification.size() << '\t'
            << classification.hitlist() << std::endl;
    }
};

void Usage(int exit_code) {
    std::cerr << "Usage: kraken2-client [options]" << std::endl
              << std::endl
              << "\t-h, -H, -?, --help           Usage" << std::endl
              << "\t-s, -S, --sequence [path]    Path to sequence file (*.fast(a|q)(.gz)" << std::endl
              << "\t-r, -R  --report   [path]    Path to output report file" << std::endl
              << "\t-i, -I  --host-ip            Server IP address (default: localhost)." << std::endl
              << "\t-p, -P, --port [num]         Server port (default: 8080)." << std::endl
              << "\t-k, -K, --shutdown           Shutdown server" << std::endl
              << std::endl
              << "Leave sequence blank to request the total summary data from the specified endpoint." << std::endl
              << std::endl;
    exit(exit_code);
}

void ParseCommandLine(int argc, char **argv, Options &opts) {
    // Define the long shell arguments
    struct option long_options[] =
        {
            {"sequence", required_argument, NULL, 's'},
            {"sequence", required_argument, NULL, 'S'},
            {"report", required_argument, NULL, 'r'},
            {"report", required_argument, NULL, 'R'},
            {"host-ip", required_argument, NULL, 'i'},
            {"host-ip", required_argument, NULL, 'I'},
            {"port", required_argument, NULL, 'p'},
            {"port", required_argument, NULL, 'P'},
            {"shutdown", no_argument, NULL, 'k'},
            {"shutdown", no_argument, NULL, 'K'},
            {"help", no_argument, NULL, 'h'},
            {"help", no_argument, NULL, 'H'},
            {NULL, 0, NULL, 0}};
    int opt;
    // Handle the various shell arguments (long mapped to short)
    while ((opt = getopt_long(argc, argv, "hH?u:U:s:S:r:R:p:P:bBkK", long_options, NULL)) != -1) {
        switch (opt)
        {
        case 'h':
        case '?':
        case 'H':
            Usage(0);
            break;
        case 's':
        case 'S':
            opts.sequence = optarg;
            break;
        case 'r':
        case 'R':
            opts.report_file = optarg;
            break;
        case 'k':
        case 'K':
            opts.shutdown = true;
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
        }
    }
}

int main(int argc, char **argv) {
    Options opts;
    ParseCommandLine(argc, argv, opts);

    int rtn_code = 0;
    std::string server_address = opts.host + ":" + std::to_string(opts.port);

    std::cerr << "Connecting to server: " << server_address << "." << std::endl;
    SequenceClient client(grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials()));

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
