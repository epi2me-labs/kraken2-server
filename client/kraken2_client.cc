#include <sysexits.h>
#include <getopt.h>
#include <fstream>

#include <grpc/grpc.h>
#include <grpc++/channel.h>
#include <grpc++/client_context.h>
#include <grpc++/create_channel.h>
#include <grpc++/security/credentials.h>

#include "utils.h"
#include "Kraken2.grpc.pb.h"

#include <zlib.h>
#include "kseq.h"

KSEQ_INIT(gzFile, gzread)

using grpc::Channel;
using grpc::ClientContext;
using grpc::ClientReaderWriter;
using grpc::ClientWriter;
using grpc::Status;

using kraken2proto::Kraken2SequenceRequest;
using kraken2proto::Kraken2SequenceResult;
using kraken2proto::Kraken2SequenceResults;
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
    bool batch = false;
    bool shutdown = false;
};

class SequenceClient
{
public:
    SequenceClient(std::shared_ptr<Channel> channel) : sequence_stub(Kraken2Service::NewStub(channel)) {}

    /**
     * @brief Send a batch of sequences from a kseq file and receive a unary response with all classifications.
     *
     * @param sequence_name
     * @return true if response status from the server is OK
     * @return false if response status from the server is not OK
     */
    bool ClassifyBatch(const std::string &sequence_name, const std::string &report_file)
    {
        // Extract (and decompress if necessary) the sequences from the kseq file.
        std::cerr << "Extracting sequences from file: " + sequence_name << std::endl;
        std::vector<Kraken2SequenceRequest> seqs;
        if (!ExtractSequencesFromFileKseq(sequence_name, seqs))
            return false;
        std::cerr << "Sequences extracted successfully." << std::endl;

        // Call the relevant endpoint and write all sequences.
        std::cerr << "Uploading sequences..." << std::endl;
        ClientContext context;
        Kraken2SequenceResults response;
        std::shared_ptr<ClientWriter<Kraken2SequenceRequest>> put_sequence_writer(sequence_stub->ClassifyBatch(&context, &response));
        try
        {
            for (Kraken2SequenceRequest &s : seqs)
            {
                put_sequence_writer->Write(s);
            }
            // Signal to the server that writing is complete.
            put_sequence_writer->WritesDone();
        }
        catch (const std::exception &ex)
        {
            std::cerr << "Failed to send sequences"
                      << ": " << ex.what() << std::endl;
            return false;
        }
        std::cerr << "Sequences uploaded.\nAwaiting classification results..." << std::endl;

        // Handle the stream response
        Status status = HandleResponse(put_sequence_writer, &response, report_file);
        if (!status.ok())
        {
            std::cerr << "Sequence Batch RPC failed: " << status.error_message() << std::endl;
            return false;
        }
        return true;
    }

    /**
     * @brief Send sequences from a kseq file as a stream and receive classifications individually as a stream.
     *
     * @param sequence_name
     * @return true if response status from the server is OK once streaming has concluded.
     * @return false if response status from the server is not OK.
     */
    bool ClassifySequences(const std::string &sequence_name, const std::string &report_file)
    {
        // Extract (and decompress if necessary) the sequences from the kseq file.
        std::vector<Kraken2SequenceRequest> seqs;
        std::cerr << "Extracting sequences from file: " + sequence_name << std::endl;
        if (!ExtractSequencesFromFileKseq(sequence_name, seqs))
            return false;
        std::cerr << "Sequences extracted successfully." << std::endl;

        // Call the relevant endpoint and write all sequences.
        ClientContext context;
        std::shared_ptr<ClientReaderWriter<Kraken2SequenceRequest, Kraken2SequenceStreamResult>> put_sequence_writer(sequence_stub->ClassifyStream(&context));
        std::cerr << "Uploading sequences..." << std::endl;
        try
        {
            for (Kraken2SequenceRequest &s : seqs)
            {
                put_sequence_writer->Write(s);
            }
            // Signal to the server that writing is complete.
            put_sequence_writer->WritesDone();
        }
        catch (const std::exception &ex)
        {
            std::cerr << "Failed to send sequences"
                      << ": " << ex.what() << std::endl;
        }
        std::cerr << "Sequences uploaded.\nAwaiting classification results..." << std::endl;

        // Handle the classfication responses.
        Kraken2SequenceStreamResult result;
        // Read until the server signals writing has concluded.
        while (put_sequence_writer->Read(&result))
        {
            if (result.has_classification())
                PrintClassification(result.classification());
        }
        if (result.has_summary())
            PrintSummary(result.summary(), report_file);

        // Handle the stream response
        Status status = put_sequence_writer->Finish();
        if (!status.ok())
        {
            std::cerr << "Sequence Stream RPC failed: " << status.error_message() << std::endl;
            return false;
        }
        return true;
    }

    /**
     * @brief Request a summary of the classification history on the server.
     *
     * @return true if response status from the server is OK.
     * @return false if response status from the server is not OK.
     */
    bool GetSummary()
    {
        ClientContext context;
        Kraken2SummaryRequest req;
        Kraken2SummaryResults response;

        Status status = sequence_stub->GetSummary(&context, req, &response);
        if (!status.ok())
        {
            std::cerr << "Could not retrieve Kraken2 server summary." << std::endl;
            return false;
        }
        std::cout << response.summary() << std::endl;
        return true;
    }

    bool ShutdownServer()
    {
        ClientContext context;
        Kraken2ShutdownRequest req;
        Kraken2ShutdownResult response;
        Status status = sequence_stub->RemoteShutdown(&context, req, &response);
        if (!status.ok())
        {
            std::cerr << "Failed to send shutdown request." << std::endl;
            return false;
        }
        if (response.successful())
        {
            std::cerr << "Shutdown request processed." << std::endl;
            return true;
        }
        else
        {
            std::cerr << "Shutdown request not processed correctly." << std::endl;
            return false;
        }
    }

private:
    // The gRPC service stub for the service defined in Kraken2.proto
    std::unique_ptr<kraken2proto::Kraken2Service::Stub> sequence_stub;

    Status HandleResponse(
        std::shared_ptr<ClientWriter<Kraken2SequenceRequest>> put_sequence_writer,
        Kraken2SequenceResults *response,
        const std::string &report_file)
    {
        Status status = put_sequence_writer->Finish();
        if (status.ok())
        {
            PrintClassifications(response->classifications());
            PrintSummary(response->summary(), report_file);
        }
        return status;
    }

    void PrintSummary(const std::string &summary, const std::string &report_file)
    {
        if (report_file != "")
        {
            try
            {
                std::ofstream summary_file(report_file, std::ofstream::out);
                summary_file << summary;
                summary_file.close();
            }
            catch (const std::exception &ex)
            {
                std::cerr << "Failed to write report file"
                          << ": " << ex.what() << std::endl;
            }
        }
    }

    /**
     * @brief Prints every {modulo}th classification in the map.
     *
     * @param classifications
     * @param modulo
     */
    void PrintClassifications(const google::protobuf::Map<std::string, Kraken2SequenceResult> classifications)
    {
        for (google::protobuf::MapPair<std::string, Kraken2SequenceResult> classify : classifications)
        {
            std::string key = classify.first;
            Kraken2SequenceResult val = classify.second;
            PrintClassification(val);
        }
    }

    /**
     * @brief Print the classification.
     *
     * @param classification
     */
    void PrintClassification(Kraken2SequenceResult classification)
    {
        std::string classified = classification.classified() ? "C" : "U";
        std::cout
            << classified << '\t'
            << classification.id() << '\t'
            << classification.tax_id() << '\t'
            << classification.size() << '\t'
            << classification.hitlist() << std::endl;
    }

    bool ExtractSequencesFromFileKseq(std::string sequence_name, std::vector<Kraken2SequenceRequest> &seqs)
    {
        gzFile fp;
        kseq_t *seq;
        fp = gzopen(sequence_name.c_str(), "r");
        seq = kseq_init(fp);
        while (kseq_read(seq) >= 0)
        {
            std::string header;
            seq->qual.l == 0 ? header.append(">") : header.append("@");
            header.append(seq->name.s);
            header.append(" ");
            header.append(seq->comment.s);

            std::string str_rep;
            str_rep.append(header);
            str_rep.append("\n");

            Kraken2SequenceRequest rec;
            rec.set_id(seq->name.s);
            rec.set_seq(seq->seq.s);
            rec.set_header(header);
            if (seq->qual.l == 0)
            {
                rec.set_format(Kraken2SequenceRequest::FORMAT_FASTA);
                rec.set_quals("");
                str_rep.append(rec.seq());
                str_rep.append("\n");
            }
            else
            {
                rec.set_format(Kraken2SequenceRequest::FORMAT_FASTQ);
                rec.set_quals(seq->qual.s);
                str_rep.append(rec.seq());
                str_rep.append("\n+\n");
                str_rep.append(rec.quals());
                str_rep.append("\n");
            }
            rec.set_str_representation(str_rep);
            seqs.push_back(std::move(rec));
        }
        kseq_destroy(seq);
        gzclose(fp);
        return true;
    }
};

void Usage(int exit_code)
{
    std::cerr << "Usage: kraken2-client [options]" << std::endl
              << std::endl
              << "\t-h, -H, -?, --help           Usage" << std::endl
              << "\t-s, -S, --sequence [path]    Path to sequence file (*.fast(a|q)(.gz)" << std::endl
              << "\t-r, -R  --report   [path]    Path to output report file" << std::endl
              << "\t-i, -I  --host-ip            Server IP address (default: localhost)." << std::endl
              << "\t-p, -P, --port [num]         Server port (default: 8080)." << std::endl
              << "\t-b, -B, --batch              Upload the sequences as a batch and receive one response, rather than a stream" << std::endl
              << "\t-k, -K, --shutdown           Shutdown server" << std::endl
              << std::endl
              << "Leave sequence blank to request the total summary data from the specified endpoint." << std::endl
              << std::endl;
    exit(exit_code);
}

void ParseCommandLine(int argc, char **argv, Options &opts)
{
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
            {"batch", required_argument, NULL, 'b'},
            {"batch", required_argument, NULL, 'B'},
            {"shutdown", no_argument, NULL, 'k'},
            {"shutdown", no_argument, NULL, 'K'},
            {"help", no_argument, NULL, 'h'},
            {"help", no_argument, NULL, 'H'},
            {NULL, 0, NULL, 0}};
    int opt;
    // Handle the various shell arguments (long mapped to short)
    while ((opt = getopt_long(argc, argv, "hH?u:U:s:S:r:R:p:P:bB", long_options, NULL)) != -1)
    {
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
        case 'b':
        case 'B':
            opts.batch = true;
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

int main(int argc, char **argv)
{
    Options opts;
    ParseCommandLine(argc, argv, opts);

    bool succeeded = false;
    std::string server_address = opts.host + ":" + std::to_string(opts.port);

    std::cerr << "Connecting to server: " << server_address << "." << std::endl;
    SequenceClient client(grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials()));

    if (opts.shutdown)
    {
        succeeded = client.ShutdownServer();
    }
    else if (opts.sequence.empty())
    {
        succeeded = client.GetSummary();
    }
    else
    {
        const std::string filename(opts.sequence);
        const std::string report_file(opts.report_file);
        succeeded = opts.batch
            ? client.ClassifyBatch(filename, report_file)
            : client.ClassifySequences(filename, report_file);
    }

    return succeeded ? EX_OK : EX_IOERR;
}
