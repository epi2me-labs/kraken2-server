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
#include <iomanip>
#include <mutex>

#include "kraken2_data.h"
#include "taxonomy.h"
#include "kv_store.h"
#include "compact_hash.h"
#include "mmscanner.h"
#include "seqreader.h"
#include "aa_translate.h"
#include "utilities.h"

#include "report_server.h"
#include "Kraken2.grpc.pb.h"

using namespace kraken2;

using kraken2proto::Kraken2SequenceRequest;
using kraken2proto::Kraken2SequenceResults;
using kraken2proto::Kraken2SequenceResult;
using kraken2proto::Kraken2Service;

static const taxid_t AMBIGUOUS_SPAN_TAXON = TAXID_MAX - 2;
static const taxid_t MATE_PAIR_BORDER_TAXON = TAXID_MAX;
static const taxid_t READING_FRAME_BORDER_TAXON = TAXID_MAX - 1;

struct Options
{
    string db_path;
    int port = 8080;
    int max_queue = 0;

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
};

struct ClassificationStats
{
    uint64_t total_sequences;
    uint64_t total_bases;
    uint64_t total_classified;
};

class Kraken2ServerClassifier
{

public:
    Kraken2ServerClassifier(int argc, char **argv, Options &options);
    ~Kraken2ServerClassifier();

    bool ProcessSequence(std::vector<Sequence> &seqs, std::string &results, std::map<string, Kraken2SequenceResult> &classifications);
    const char *GetSummary();

private:
    Options opts;
    Taxonomy taxonomy;
    CompactHashTable hash;
    IndexOptions idx_opts;
    taxon_counters_t total_taxon_counters;
    ClassificationStats total_stats = {0, 0, 0};
    std::string summary;
    std::mutex stats_mtx;
    
    int argc;
    char **argv;

    void AddHitlistString(ostringstream &oss, vector<taxid_t> &taxa,
                          Taxonomy &taxonomy);

    taxid_t ClassifySequence(Sequence &dna,
                             CompactHashTable &hash, Taxonomy &taxonomy, IndexOptions &idx_opts,
                             Options &opts, ClassificationStats &stats, MinimizerScanner &scanner,
                             vector<taxid_t> &taxa, taxon_counts_t &hit_counts,
                             vector<string> &tx_frames, taxon_counters_t &curr_taxon_counts,
                             std::map<string, Kraken2SequenceResult> &classifications);

    void MaskLowQualityBases(Sequence &dna, int minimum_quality_score);

    void ProcessFiles(std::vector<Sequence> &seqs,
                      CompactHashTable &hash, Taxonomy &tax,
                      IndexOptions &idx_opts, Options &opts, ClassificationStats &stats,
                      taxon_counters_t &total_taxon_counters,
                      std::map<string, Kraken2SequenceResult> &classifications);

    std::string ReportStats(struct timeval time1, struct timeval time2,
                            ClassificationStats &stats);

    std::string ReportTotalStats(ClassificationStats &stats);

    taxid_t ResolveTree(taxon_counts_t &hit_counts,
                        Taxonomy &taxonomy, size_t total_minimizers, Options &opts);

    std::string TrimPairInfo(std::string &id);

    std::string DoubleStatToString(double d, const int precision);
};