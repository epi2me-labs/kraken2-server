#include <iomanip>
#include <future>

// kraken2
#include "kraken2_data.h"
#include "taxonomy.h"
#include "kv_store.h"
#include "compact_hash.h"
#include "mmscanner.h"
#include "seqreader.h"
#include "aa_translate.h"
#include "utilities.h"

// kraken2 server
#include "thread_pool_light.hpp"
#include "report_server.h"
#include "thread_safe_queue.h"
#include "Kraken2.grpc.pb.h"

using namespace kraken2;

using kraken2proto::Kraken2SequenceRequest;
using kraken2proto::Kraken2SequenceResult;
using kraken2proto::Kraken2Service;

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


struct BatchResults {
   std::vector<Kraken2SequenceResult> k2results;
   taxon_counters_t taxon_counters;
   ClassificationStats stats = {0, 0, 0};
};


class Kraken2ServerClassifier {

public:
    bool index_available = false;
    bool index_broken = false;

    /**
     * @brief Construct a new Kraken 2 Server Classifier. Loads the database only once and is
     *        reused for all requests.
     */
    Kraken2ServerClassifier(Options &options);
    ~Kraken2ServerClassifier();

    /**
     * @brief Load kraken2 index
     * 
     */
    void LoadIndex();

    /**
     * @brief Classify sequences in a input queue and populate the classification queue.
     */
    void ProcessSequenceStream(
        ThreadSafeQueue<Sequence> *seqs,
        ThreadSafeQueue<Kraken2SequenceResult> *classifications,
        std::string &results,
        std::future<void> end);
    
    /**
     * @brief Classifies the vector of sequences and populates the string and map with classification
     *        summary and results respectively.
     */
    bool ProcessBatch(
        std::vector<Sequence> seqs,
        ThreadSafeQueue<BatchResults> *result_q);

    /**
     * @brief Return a summary of historical classifications.
     */
    const char *GetSummary();

private:
    // Database and Historical Stats
    Options opts;
    Taxonomy taxonomy;
    CompactHashTable hash;
    IndexOptions idx_opts;
    taxon_counters_t total_taxon_counters;
    ClassificationStats total_stats = {0, 0, 0};
    std::string summary;
    std::mutex stats_mtx;
    BS::thread_pool_light pool;

    void AddHitlistString(ostringstream &oss, vector<taxid_t> &taxa, Taxonomy &taxonomy);

    Kraken2SequenceResult ClassifySequence(
        Sequence &dna,
        CompactHashTable &hash, Taxonomy &taxonomy, IndexOptions &idx_opts,
        Options &opts, ClassificationStats &stats, MinimizerScanner &scanner,
        vector<taxid_t> &taxa, taxon_counts_t &hit_counts,
        vector<string> &tx_frames, taxon_counters_t &curr_taxon_counts);

    void MaskLowQualityBases(Sequence &dna, int minimum_quality_score);

    void ProcessFile(
        Sequence &seq,
        CompactHashTable &hash, Taxonomy &tax,
        IndexOptions &idx_opts, Options &opts, ClassificationStats &stats,
        taxon_counters_t &total_taxon_counters,
        Kraken2SequenceResult &classification,
        MinimizerScanner &scanner, vector<taxid_t> &taxa,
        taxon_counts_t &hit_counts, vector<string> &translated_frames, SequenceFormat &format);

    std::string ReportStats(struct timeval time1, struct timeval time2, ClassificationStats &stats);

    std::string ReportTotalStats(ClassificationStats &stats);

    void GenerateReport(
        std::string &results, std::string &summary, Options &opts, Taxonomy &taxonomy,
        timeval &tv1, timeval &tv2, ClassificationStats &stats, ClassificationStats &total_stats,
        taxon_counters_t &taxon_counters, taxon_counters_t &total_taxon_counters, std::mutex &stats_mtx);

    taxid_t ResolveTree(taxon_counts_t &hit_counts, Taxonomy &taxonomy, size_t total_minimizers, Options &opts);

    std::string TrimPairInfo(std::string &id);

    std::string DoubleStatToString(double d, const int precision);
};