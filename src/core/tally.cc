#include "core/tally.h"

#include <cstdio>
#include <sstream>

#include "reports.h"

namespace k2s {

namespace {

std::string Fixed(double value, int precision)
{
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.*f", precision, value);
    return buffer;
}

// part as a percentage of whole, 0 if there is nothing to take a part of
std::string Percent(uint64_t part, uint64_t whole)
{
    return Fixed(whole ? part * 100.0 / whole : 0.0, 2);
}

}  // namespace

void Tally::Merge(Tally &&other)
{
    stats += other.stats;
    for (auto &entry : other.counters)
        counters[entry.first] += std::move(entry.second);
}

std::string FormatReport(const Index &index, const Tally &tally, const ReportOptions &options)
{
    // kraken2's report functions take mutable references but only read
    kraken2::taxon_counters_t counters = tally.counters;
    kraken2::taxon_counters_t clade_counters =
        kraken2::GetCladeCounters(index.taxonomy(), counters);

    std::ostringstream out;
    // The header, which the report kraken2 writes to a file does not have
    out << "% of Seqs\tClades\tTaxonomies\t";
    if (options.kmer_data)
        out << "Kmers\tDistinct Kmers\t";
    out << "Rank\tTaxonomy ID\tScientific Name\n";

    // The unclassified sequences are handled specially
    if (tally.stats.unclassified() != 0 || options.zero_counts) {
        kraken2::READCOUNTER rc(tally.stats.unclassified(), 0);
        kraken2::PrintKrakenStyleReportLine(out, options.kmer_data, tally.stats.sequences, rc,
                                            rc, "U", 0, "unclassified", 0);
    }
    // then a depth first walk down the taxonomy
    kraken2::KrakenReportDFS(1, out, options.zero_counts, options.kmer_data, index.taxonomy(),
                             clade_counters, counters, tally.stats.sequences, 'R', -1, 0);
    return out.str();
}

std::string DescribeRun(const ClassificationStats &stats, double seconds)
{
    const double minutes = seconds > 0 ? seconds / 60 : 1;   // avoid dividing by zero for an instant run
    const double mbp = stats.bases / 1.0e6;
    return std::to_string(stats.sequences) + " sequences (" + Fixed(mbp, 2) + " Mbp) processed in " +
           Fixed(seconds, 2) + "s (" + Fixed(stats.sequences / 1.0e3 / minutes, 2) + " Kseq/m, " +
           Fixed(mbp / minutes, 2) + " Mbp/m).\n" +
           "\t" + std::to_string(stats.classified) + " sequences classified (" +
           Percent(stats.classified, stats.sequences) + "%)\n" +
           "\t" + std::to_string(stats.unclassified()) + " sequences unclassified (" +
           Percent(stats.unclassified(), stats.sequences) + "%)\n";
}

std::string DescribeTotals(const ClassificationStats &stats)
{
    return std::to_string(stats.sequences) + " sequences (" + Fixed(stats.bases / 1.0e6, 2) +
           " Mbp) processed.\n" +
           std::to_string(stats.classified) + " sequences classified (" +
           Percent(stats.classified, stats.sequences) + "%).\n" +
           std::to_string(stats.unclassified()) + " sequences unclassified (" +
           Percent(stats.unclassified(), stats.sequences) + "%).\n";
}

void History::Add(Tally &&tally)
{
    std::lock_guard<std::mutex> lock(mutex_);
    total_.Merge(std::move(tally));
}

ClassificationStats History::Stats() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return total_.stats;
}

std::string History::Summary(const Index &index, const ReportOptions &options) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (total_.stats.sequences == 0)
        return "";   // nothing has been classified yet
    return FormatReport(index, total_, options) + "\n" + DescribeTotals(total_.stats);
}

}  // namespace k2s
