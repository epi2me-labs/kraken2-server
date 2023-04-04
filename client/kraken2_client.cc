#include <atomic>
#include <cassert>
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
     * @return EX_UNAVAILABLE if sequences could nto be sent to server
     * @return else gRPC status code
     */
    int ClassifySequences(const std::string &sequence_name, const std::string &report_file) {
        std::cerr << "Classifying sequence stream." << std::endl;
        int state = WaitForServer();
        if (state != 0) {return state;}

        ClientContext context;
        Kraken2SequenceResultMulti response;
        ClientStream stream(sequence_stub->ClassifyStream(&context));
        std::atomic<uint64_t> seqs_in_flight = 0;

        // queue for gRPC messages (i.e. sequence reads)
        ThreadSafeQueue<std::vector<Kraken2SequenceRequest>>
            *batches_queue = new ThreadSafeQueue<std::vector<Kraken2SequenceRequest>>();

        // reads data from file into queue
        std::future<int> fastq_batches = std::async(
            std::launch::async, &SequenceClient::FastBatcher, this,
            std::ref(sequence_name), batches_queue);

        // take data from queue and send over gRPC
        std::future<int> stream_batches = std::async(
            std::launch::async, &SequenceClient::StreamWriter, this,
            std::ref(fastq_batches), std::ref(seqs_in_flight),
            batches_queue, std::ref(stream));

        // reading back results on gRPC stream
        std::future<int> recv_reads = std::async(
            std::launch::async, &SequenceClient::StreamReader, this,
            std::ref(seqs_in_flight), std::ref(report_file), std::ref(stream));

        // wait for things to finish in order
        fastq_batches.wait();
        stream_batches.wait();
        std::cerr << "Batches: " << fastq_batches.get() << " " << stream_batches.get() << std::endl;
        recv_reads.wait();
        std::cerr << "Done waiting" << std::endl;

        delete batches_queue;
        assert(seqs_in_flight==0);

        // Handle the stream response
        Status status = stream->Finish();
        if (!status.ok()) {
            std::cerr << "Client RPC stream failed: " << status.error_message() << std::endl;
        }
    
        return status.error_code();
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
            std::future<int> &total_batches,
            std::atomic<uint64_t> &seqs_in_flight,
            ThreadSafeQueue<std::vector<Kraken2SequenceRequest>> *batches,
            ClientStream &writer) {
        int seqs_sent = 0;
        try {
            while (!(
                // we've finished if we've received the signal AND theres nothing left
                total_batches.wait_for(0s) == std::future_status::ready
                && batches->size() == 0
            )) {
                std::optional<std::vector<Kraken2SequenceRequest>> item = batches->pop();
                if (item.has_value()) {
                    std::vector<Kraken2SequenceRequest> batch = item.value();
                    bool show_msg = true;
                    while (true) {
                        if ((seqs_in_flight + batch.size() >= MAX_IN_FLIGHT)) {
                            std::this_thread::sleep_for(10ms);
                            if (show_msg) {
                                show_msg = false;
                                std::cerr << "Waiting before sending more. In-flight: " << seqs_in_flight << "." << std::endl;
                            }
                            continue;
                        }
                        else { break; }
                    }
                    
                    // rebatch to smaller batches for stream
                    const uint64_t MAX_SIZE = 128 * 1024 * 1024;
                    for(size_t i = 0; i < batch.size(); i += ST_BATCH_SIZE) {
                        auto last = std::min(batch.size(), i + ST_BATCH_SIZE);
                        size_t bsize = last - i;
                        Kraken2SequenceRequestMulti req;
                        req.mutable_seqs()->Assign(batch.begin() + i, batch.begin() + last);
                        uint64_t msg_size = req.ByteSizeLong();
                        if (msg_size > MAX_SIZE) {
                            // send one by one
                            for (size_t k = i; k<last; ++k) {
                                Kraken2SequenceRequestMulti req;
                                req.mutable_seqs()->Assign(batch.begin() + k, batch.begin() + k + 1);
                                if (req.ByteSizeLong() > MAX_SIZE) {
                                    std::cerr << "Read is too large! Skipping." << std::endl;
                                    continue;
                                }
                                writer->Write(req, WriteOptions().set_buffer_hint());
                                seqs_in_flight.fetch_add(1);
                                seqs_sent++;
                            }
                        }
                        else {
                            writer->Write(req);
                            seqs_in_flight.fetch_add(bsize);
                            seqs_sent += bsize;
                        }
                    }
                }
            }
        }
        catch (const std::exception &ex) {
            std::cerr << "Failed to send sequences"
                      << ": " << ex.what() << std::endl;
            writer->WritesDone();
            return seqs_sent;
        }

        // TODO: HORRIBLE HACK
        //   For some reason, if the original input file contained very few
        //   reads (~1000), if we do not wait a bit here before calling
        //   writer->WritesDone() the recv_reads task in the main thread
        //   hangs and does not complete. I've not observed this with files 
        //   that contain 4000 reads.
        std::this_thread::sleep_for(500ms);
        writer->WritesDone();
        return seqs_sent;
    }

    int StreamReader(
            std::atomic<uint64_t> &seqs_in_flight, const std::string &report_file,
            ClientStream &reader) {
        Kraken2SequenceStreamResult result;
        int n_reads = 0;
        try {
            while (reader->Read(&result)) {
                if (result.has_classifications()) {
                    for (auto &res : result.classifications().classes()){
                        n_reads++;
                        PrintClassification(res);
                        seqs_in_flight--;
                    }
                }
                if (result.has_summary())
                    PrintSummary(result.summary(), report_file);
            }
        }
        catch (const std::exception &ex) {
            std::cerr << "Failed to receive responses"
                      << ": " << ex.what() << std::endl;
            return n_reads;
        }
        return n_reads;
    }
    
    int FastBatcher(
            const std::string &sequence_file,
            ThreadSafeQueue<std::vector<Kraken2SequenceRequest>> *batches_queue) {
        int n_batches = 0;
        try {
            FastReader reader = FastReader(sequence_file);
        
            std::cerr << "Reading sequences from file: " << sequence_file << std::endl;
            while (true) {
                if ((batches_queue->size() >= MAX_BATCHES)) {
                    std::cerr << "Waiting before reading new read batch." << std::endl;
                    std::this_thread::sleep_for(10ms);
                    continue;
                }
 
                std::vector<Kraken2SequenceRequest> seqs;
                int n_reads;
                if ((n_reads = reader.read(seqs, FASTQ_BATCH_SIZE)) > 0) {
                    n_batches++;
                    batches_queue->push(std::move(seqs));
                }
                else { break; }
            }
        }
        catch (const std::exception &ex) {
            std::cerr << "Failed to read sequences from file: " << sequence_file
                      << ": " << ex.what() << std::endl;
        }
        return n_batches;
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
