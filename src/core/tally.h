#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "core/index.h"
#include "readcounts.h"

namespace k2s {

/// How many reads and bases were processed, and how many of the reads were classified.
struct ClassificationStats
{
    uint64_t sequences = 0;
    uint64_t bases = 0;
    uint64_t classified = 0;

    uint64_t unclassified() const { return sequences - classified; }

    ClassificationStats &operator+=(const ClassificationStats &other)
    {
        sequences += other.sequences;
        bases += other.bases;
        classified += other.classified;
        return *this;
    }
};

/// The outcome of classifying some reads: the totals, and what was found
/// for each taxon. Tallies from separate batches can be merged.
struct Tally
{
    ClassificationStats stats;
    kraken2::taxon_counters_t counters;

    /// Add `other` into this, taking its taxon counters.
    void Merge(Tally &&other);
};

struct ReportOptions
{
    /// List taxa that nothing was classified as
    bool zero_counts = false;
    /// Include k-mer counts (the classifier must have been keeping them)
    bool kmer_data = false;
};

/// A report in kraken2's format, with a header line.
std::string FormatReport(const Index &index, const Tally &tally, const ReportOptions &options);

/// "N sequences (X Mbp) processed in Ys (...)", for a run that took `seconds`.
std::string DescribeRun(const ClassificationStats &stats, double seconds);

/// "N sequences (X Mbp) processed", for a running total.
std::string DescribeTotals(const ClassificationStats &stats);

/**
 * Everything the server has classified since it started: the totals, and for
 * each taxon what was found. Safe to use from many threads.
 */
class History
{
public:
    void Add(Tally &&tally);

    ClassificationStats Stats() const;

    /// The report over everything so far, followed by the totals.
    std::string Summary(const Index &index, const ReportOptions &options) const;

private:
    mutable std::mutex mutex_;
    Tally total_;
};

}  // namespace k2s
