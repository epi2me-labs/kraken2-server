#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "aa_translate.h"
#include "core/index.h"
#include "kraken2_data.h"
#include "mmscanner.h"
#include "readcounts.h"

namespace k2s {

/// Settings that change what a read is classified as.
struct ClassifyParams
{
    /// Fraction of a read's minimizers that must support the call (0 - 1)
    double confidence_threshold = 0.0;
    /// FASTQ bases with a quality below this are ignored (0 to use every base)
    int minimum_quality_score = 0;
    /// Distinct minimizers that must hit the database for a read to be called
    int minimum_hit_groups = 2;
    /// Keep per-taxon k-mer counts, for reports that include them
    bool report_kmer_data = false;
};

/// One sequence to classify. Only views: the data must outlive the call.
struct Read
{
    std::string_view id;
    std::string_view seq;
    std::string_view quals;  // FASTQ only
    bool fastq = false;
};

/// What was found for one read.
struct Classification
{
    /// The taxon in the database's own numbering; 0 if the read was not classified.
    /// Use Classifier::ExternalId() to get the taxonomy (NCBI) id.
    kraken2::taxid_t taxon = 0;
    /// How each stretch of the read matched: (taxon, count) pairs, run-length
    /// encoded. Reading frame and ambiguity markers are mixed in. Use
    /// Classifier::AppendHitlist() to print it.
    std::vector<kraken2::taxid_t> hit_runs;
};

/**
 * Classifies reads against an Index.
 *
 * A Classifier holds scratch space that is reused from read to read, so it is
 * not thread safe: use one per thread. They are cheap to make, and any number
 * can share an Index.
 */
class Classifier
{
public:
    Classifier(const Index &index, const ClassifyParams &params);

    /**
     * Classify a read.
     *
     * @param out receives the result; reuse one across calls to avoid allocating
     * @param counters the read's call, and optionally its k-mers, are added here
     * @return whether the read was classified
     * @throws std::invalid_argument if quality masking is on and a FASTQ read's
     *         quality string is not as long as its sequence
     */
    bool Classify(const Read &read, Classification &out, kraken2::taxon_counters_t &counters);

    /// The taxonomy (NCBI) id of a taxon in the database's numbering.
    uint64_t ExternalId(kraken2::taxid_t taxon) const;

    /// The scientific name of a taxon.
    const char *Name(kraken2::taxid_t taxon) const;

    /// Append the hitlist in kraken2's text form, e.g. "0:12 1423:30 A:2" ("0:0" if the
    /// read had no minimizers).
    void AppendHitlist(const Classification &c, std::string &out) const;

private:
    const Index &index_;
    const ClassifyParams params_;
    kraken2::MinimizerScanner scanner_;
    // scratch, reused between reads
    std::vector<std::string> frames_;
    kraken2::taxon_counts_t hit_counts_;
    std::vector<kraken2::LookupData> lookup_buffer_;
    std::string masked_;
};

}  // namespace k2s
