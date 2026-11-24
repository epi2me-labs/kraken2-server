#pragma once

#include <ostream>

#include "reports.h"

namespace kraken2
{
    // Stream-based variant of upstream's file-based ReportKrakenStyle, which
    // lets the server build reports in memory. Everything else in the report
    // (clade counting, the DFS, line formatting) is upstream's, from reports.h.
    void ReportKrakenStyle(std::ostream &ss, bool report_zeros, bool report_kmer_data,
                           Taxonomy &taxonomy, taxon_counters_t &call_counters,
                           uint64_t total_seqs, uint64_t total_unclassified);
}
