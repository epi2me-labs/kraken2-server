#ifndef KRAKEN2_REPORTS_H_
#define KRAKEN2_REPORTS_H_

#include <unordered_map>
#include <iomanip>
#include "kraken2_headers.h"
#include "taxonomy.h"
#include "kraken2_data.h"
#include "readcounts.h"

namespace kraken2 {

void PrintKrakenStyleReportLine(std::ostringstream &ofs, bool report_kmer_data,
    uint64_t total_seqs, READCOUNTER clade_counter, READCOUNTER taxon_counter,
    const std::string &rank_str, uint32_t taxid, const std::string &sci_name, int depth);
void KrakenReportDFS(uint32_t taxid, std::ostringstream &ofs, bool report_zeros,
    bool report_kmer_data, Taxonomy &taxonomy, taxon_counters_t &clade_counters,
    taxon_counters_t &call_counters, uint64_t total_seqs, char rank_code, int rank_depth, int depth);
void ReportKrakenStyle(std::ostringstream &ss, bool report_zeros, bool report_kmer_data,
    Taxonomy &taxonomy, taxon_counters_t &call_counters, uint64_t total_seqs, uint64_t total_unclassified);

}

#endif
