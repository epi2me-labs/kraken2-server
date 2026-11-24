#include <iomanip>
#include <atomic>
#include <future>
#include <mutex>
#include <memory>
#include <thread>

// kraken2
#include "kraken2_data.h"
#include "taxonomy.h"
#include "kv_store.h"
#include "compact_hash.h"
#include "mmscanner.h"
#include "fast_reader.h"
#include "sequence.h"
#include "aa_translate.h"
#include "utilities.h"

// kraken2 server
#include "thread_pool.hpp"
#include "report_server.h"
#include "thread_safe_queue.h"
#include "Kraken2.grpc.pb.h"

using namespace kraken2;
using k2server::Sequence;

using grpc::ServerContext;
using grpc::ServerReaderWriter;
using grpc::WriteOptions;

using kraken2proto::Kraken2Service;
using kraken2proto::Kraken2SequenceRequest;
using kraken2proto::Kraken2SequenceRequestMulti;
using kraken2proto::Kraken2SequenceResult;
using kraken2proto::Kraken2SequenceResultMulti;
using kraken2proto::Kraken2SequenceStreamResult;

typedef ServerReaderWriter<Kraken2SequenceStreamResult, Kraken2SequenceRequestMulti> ServerStream;

static const taxid_t AMBIGUOUS_SPAN_TAXON = TAXID_MAX - 2;
static const taxid_t MATE_PAIR_BORDER_TAXON = TAXID_MAX;
static const taxid_t READING_FRAME_BORDER_TAXON = TAXID_MAX - 1;


struct Options {
    string db_path;
    string host = "localhost";
    int port = 8080;
    int max_queue = 0;
    int thread_pool = 1;

    string index_filename;
    string taxonomy_filename;
    string options_filename;
    string report_filename = "latest_run.txt";
    bool report_kmer_data = false;
    bool report_zero_counts = false;
    bool use_translated_search = false;
    bool stats = true;
    double confidence_threshold = 0.0;
    int minimum_quality_score = 0;
    int minimum_hit_groups = 2;
    bool use_memory_mapping = false;
    int wait = 0;
};


struct ClassificationStats {
    uint64_t total_sequences;
    uint64_t total_bases;
    uint64_t total_classified;
};


/**
 * @brief Records the first error raised while classifying a stream, so worker
 *        threads can report it without throwing.
 */
class StreamFailure {
public:
    void Set(const std::string &message) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!failed_.exchange(true))
            message_ = message;
    }
    bool failed() const { return failed_.load(); }
    std::string message() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return message_;
    }
private:
    std::atomic<bool> failed_{false};
    mutable std::mutex mtx_;
    std::string message_;
};


struct BatchResults {
   Kraken2SequenceResultMulti k2results;
   taxon_counters_t taxon_counters;
   ClassificationStats stats = {0, 0, 0};
};


enum class IndexState { Loading, Ready, Failed };


class Kraken2ServerClassifier {

public:
    /**
     * @brief Construct a new Kraken 2 Server Classifier. The database is loaded once,
     *        in the background, and then reused for all requests: check state()
     *        before classifying.
     */
    explicit Kraken2ServerClassifier(const Options &options);
    ~Kraken2ServerClassifier();

    Kraken2ServerClassifier(const Kraken2ServerClassifier &) = delete;
    Kraken2ServerClassifier &operator=(const Kraken2ServerClassifier &) = delete;

    IndexState state() const { return state_.load(); }

    /**
     * @brief Classify the sequences arriving on a stream, writing classifications
     *        back as they are produced. On success `report` holds the Kraken-style
     *        report for the stream.
     * @return OK, or INVALID_ARGUMENT if a sequence could not be classified.
     */
    grpc::Status ProcessSequenceStream(
        ServerContext *context, ServerStream *stream, std::string &report);
    
    /**
     * @brief Classifies a batch of sequences and pushes the results to the queue.
     *        Failures are recorded in `failure` rather than thrown, as this runs
     *        on thread pool threads.
     */
    void ProcessBatch(
        Kraken2SequenceRequestMulti reqs,
        ThreadSafeQueue<BatchResults> *result_q,
        StreamFailure *failure);

    /**
     * @brief Return a summary of historical classifications.
     */
    std::string GetSummary() const;

private:
    // Database and Historical Stats
    Options opts;
    std::atomic<IndexState> state_{IndexState::Loading};
    std::thread loader_;
    // Set by the loader thread, before state_ becomes Ready
    std::unique_ptr<Taxonomy> taxonomy;
    // 32- or 40-bit cell table, chosen from the database file at load time
    std::unique_ptr<KeyValueStore> hash;
    IndexOptions idx_opts;
    taxon_counters_t total_taxon_counters;
    ClassificationStats total_stats = {0, 0, 0};
    std::string summary;
    mutable std::mutex stats_mtx;  // guards summary, total_stats, total_taxon_counters
    BS::thread_pool pool;

    /// Load the database files. Runs on loader_.
    void LoadIndex();

    void AddHitlistString(std::string &oss, vector<taxid_t> &taxa, Taxonomy &taxonomy);

    Kraken2SequenceResult ClassifySequence(
        Sequence &dna,
        KeyValueStore *hash, Taxonomy &taxonomy, IndexOptions &idx_opts,
        Options &opts, ClassificationStats &stats, MinimizerScanner &scanner,
        vector<taxid_t> &taxa, taxon_counts_t &hit_counts,
        vector<string> &tx_frames, taxon_counters_t &curr_taxon_counts);

    void MaskLowQualityBases(Sequence &dna, int minimum_quality_score);

    std::string ReportStats(struct timeval time1, struct timeval time2, ClassificationStats &stats);

    std::string ReportTotalStats(ClassificationStats &stats);

    /**
     * @brief Build the report for one stream, and fold the stream's results
     *        into the server's history (when tracked).
     */
    void GenerateReport(
        std::string &report, timeval &tv1, timeval &tv2,
        ClassificationStats &stats, taxon_counters_t &taxon_counters);

    taxid_t ResolveTree(taxon_counts_t &hit_counts, Taxonomy &taxonomy, size_t total_minimizers, Options &opts);

    std::string DoubleStatToString(double d, const int precision);
};
